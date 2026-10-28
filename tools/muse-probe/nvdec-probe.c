/*
 * nvdec-probe — read-only inspection of muse-server's V4L2 video decoder.
 *
 * Purpose (Nuvio libass "video lane" Phase 0, see claude/libass-video-lane-PLAN.md):
 *   Answer "what format / size / memory model does the decoder output, and can
 *   we reach those frames" WITHOUT changing anything on the TV. Needed because
 *   the S95C has no v4l2-ctl / strace / gdb.
 *
 * This tool ONLY issues V4L2 *get* ioctls (QUERYCAP / ENUM_FMT / G_FMT /
 * G_SELECTION). It never sets a format, allocates/queues buffers, starts or
 * stops streaming, or writes to any device. Nothing it does changes decode
 * state or persists across a reboot.
 *
 * It is built freestanding (no libc) as a static armv7 EABI binary so it has
 * zero dependency on Tizen's userland and can be dropped onto the TV and run
 * as root via vetro /exec.
 *
 * Three layers, least intrusive first:
 *   Layer 0  (always) PASSIVE: locate muse-server, confirm its /dev/videoN fd
 *            from /proc, dump the matching sysfs name + any debugfs nodes.
 *            Touches muse-server not at all; pure /proc + /sys reads.
 *   Layer 1  (default) INDEPENDENT OPEN: open a *separate* handle to the same
 *            /dev/videoN (a new M2M context, does not disturb muse-server's)
 *            and read QUERYCAP + ENUM_FMT (OUTPUT=coded, CAPTURE=decoded) +
 *            default G_FMT. Shows the decoder's capabilities and the pixel
 *            formats it can emit. Read-only.
 *   Layer 2  (--live, opt-in) LIVE READ via ptrace: attach to muse-server,
 *            inject *read-only* G_FMT / G_SELECTION on its real decoder fd to
 *            read the actually-negotiated size/format for the playing stream,
 *            then restore registers+memory and detach. Momentarily stops the
 *            muse-server main thread (like any debugger). If anything is off it
 *            detaches without injecting. muse-server is Restart=always, so the
 *            worst realistic failure is a brief A/V glitch, not a brick.
 *
 * Usage on the TV (as root):
 *   nvdec-probe            # Layer 0 + 1  (safe; run this first)
 *   nvdec-probe --live     # also Layer 2 (reads muse-server's live fd)
 *   nvdec-probe --dev /dev/video61   # force a device node for Layer 1
 *
 * armv7 only. Integer syscalls only; no float.
 */

/* ---- fixed width types ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef int                s32;
typedef unsigned long long u64;
typedef long long          s64;

/* ---- arm EABI syscall numbers ---- */
#define SYS_exit_group 248
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_lseek      19
#define SYS_readlink   85
#define SYS_getdents64 217
#define SYS_ioctl      54
#define SYS_ptrace     26
#define SYS_wait4     114
#define SYS_nanosleep 162

#define O_RDONLY 0
#define O_RDWR   2
#define O_DIRECTORY 040000

static inline long sc(long n, long a, long b, long c, long d, long e)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    __asm__ volatile("svc 0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
                     : "memory");
    return r0;
}
#define sc0(n)              sc((n),0,0,0,0,0)
#define sc1(n,a)            sc((n),(long)(a),0,0,0,0)
#define sc2(n,a,b)          sc((n),(long)(a),(long)(b),0,0,0)
#define sc3(n,a,b,c)        sc((n),(long)(a),(long)(b),(long)(c),0,0)
#define sc4(n,a,b,c,d)      sc((n),(long)(a),(long)(b),(long)(c),(long)(d),0)

static long sys_write(int fd, const void *b, long n){ return sc3(SYS_write, fd, b, n); }
static long sys_read (int fd, void *b, long n){ return sc3(SYS_read, fd, b, n); }
static int  sys_open (const char *p, int fl){ return (int)sc3(SYS_open, p, fl, 0); }
static int  sys_close(int fd){ return (int)sc1(SYS_close, fd); }
static long sys_readlink(const char *p, char *b, long n){ return sc3(SYS_readlink, p, b, n); }
static long sys_getdents64(int fd, void *b, long n){ return sc3(SYS_getdents64, fd, b, n); }
static long sys_ioctl(int fd, unsigned long req, void *arg){ return sc3(SYS_ioctl, fd, (long)req, arg); }
static long sys_ptrace(long req, long pid, long addr, long data){ return sc4(SYS_ptrace, req, pid, addr, data); }
static long sys_wait4(long pid, int *st, int opt){ return sc4(SYS_wait4, pid, st, opt, 0); }
static void sys_exit(int code){ sc1(SYS_exit_group, code); }

/* compiler may still emit these for aggregate init/zeroing */
void *memset(void *d, int c, unsigned long n){ u8 *p=d; while(n--) *p++=(u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n){ u8 *a=d; const u8 *b=s; while(n--) *a++=*b++; return d; }
int   memcmp(const void *a, const void *b, unsigned long n){ const u8 *x=a,*y=b; while(n--){ if(*x!=*y) return *x-*y; x++; y++; } return 0; }

/* ---- tiny string helpers ---- */
static unsigned slen(const char *s){ unsigned n=0; while(s[n]) n++; return n; }
static int streq(const char *a, const char *b){ while(*a&&*b){ if(*a!=*b) return 0; a++; b++; } return *a==*b; }
static int sstarts(const char *s, const char *p){ while(*p){ if(*s!=*p) return 0; s++; p++; } return 1; }

/* ---- output buffer (flushed once at exit) ---- */
static char OUT[200000];
static unsigned OUTN = 0;
static void o(const char *s){ unsigned n=slen(s); if(OUTN+n>sizeof(OUT)) n=sizeof(OUT)-OUTN; memcpy(OUT+OUTN,s,n); OUTN+=n; }
static void oc(char c){ if(OUTN<sizeof(OUT)) OUT[OUTN++]=c; }
static void onl(void){ oc('\n'); }
static void ou(u32 v){ char t[12]; int i=0; if(!v){ oc('0'); return;} while(v){ t[i++]='0'+v%10; v/=10;} while(i--) oc(t[i]); }
static void oi(s32 v){ if(v<0){ oc('-'); ou((u32)(-v)); } else ou((u32)v); }
static void ox(u32 v){ const char *h="0123456789abcdef"; oc('0'); oc('x'); for(int i=28;i>=0;i-=4) oc(h[(v>>i)&0xf]); }
/* print a pixelformat fourcc (u32) as chars */
static void ofourcc(u32 f){ for(int i=0;i<4;i++){ char c=(char)((f>>(8*i))&0xff); if(c>=32&&c<127) oc(c); else { oc('['); ou((u8)c); oc(']'); } } oc(' '); oc('('); ox(f); oc(')'); }
static void flush(void){ sys_write(1, OUT, OUTN); }

/* ---- V4L2 UAPI (faithful fixed-width definitions; _IOC computed for THIS target) ---- */
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT+_IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir,type,nr,size) (((dir)<<_IOC_DIRSHIFT)|((type)<<_IOC_TYPESHIFT)|((nr)<<_IOC_NRSHIFT)|((size)<<_IOC_SIZESHIFT))
#define _IOR(type,nr,sz)  _IOC(_IOC_READ,(type),(nr),sizeof(sz))
#define _IOWR(type,nr,sz) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(sz))

struct v4l2_capability {
    u8  driver[16];
    u8  card[32];
    u8  bus_info[32];
    u32 version;
    u32 capabilities;
    u32 device_caps;
    u32 reserved[3];
};
struct v4l2_fmtdesc {
    u32 index;
    u32 type;
    u32 flags;
    u8  description[32];
    u32 pixelformat;
    u32 reserved[4];
};
struct v4l2_pix_format {
    u32 width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv, flags;
    u32 enc; u32 quantization, xfer_func;
};
struct v4l2_plane_pix_format {
    u32 sizeimage;
    u32 bytesperline;
    u16 reserved[6];
};
struct v4l2_pix_format_mplane {
    u32 width, height, pixelformat, field, colorspace;
    struct v4l2_plane_pix_format plane_fmt[8];
    u8  num_planes;
    u8  flags;
    u8  enc;             /* ycbcr_enc / hsv_enc union */
    u8  quantization;
    u8  xfer_func;
    u8  reserved[7];
};
struct v4l2_format {
    u32 type;
    union {
        struct v4l2_pix_format        pix;
        struct v4l2_pix_format_mplane pix_mp;
        u8 raw_data[200];
    } fmt;
};
struct v4l2_rect { s32 left, top; u32 width, height; };
struct v4l2_selection {
    u32 type;
    u32 target;
    u32 flags;
    struct v4l2_rect r;
    u32 reserved[9];
};

#define VIDIOC_QUERYCAP    _IOR('V', 0, struct v4l2_capability)
#define VIDIOC_ENUM_FMT    _IOWR('V', 2, struct v4l2_fmtdesc)
#define VIDIOC_G_FMT       _IOWR('V', 4, struct v4l2_format)
#define VIDIOC_G_SELECTION _IOWR('V', 25, struct v4l2_selection)   /* 0x19 */

#define V4L2_BUF_TYPE_VIDEO_CAPTURE         1
#define V4L2_BUF_TYPE_VIDEO_OUTPUT          2
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE  9
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE  10

#define V4L2_CAP_VIDEO_CAPTURE        0x00000001
#define V4L2_CAP_VIDEO_OUTPUT         0x00000002
#define V4L2_CAP_VIDEO_CAPTURE_MPLANE 0x00001000
#define V4L2_CAP_VIDEO_OUTPUT_MPLANE  0x00002000
#define V4L2_CAP_VIDEO_M2M            0x00008000
#define V4L2_CAP_VIDEO_M2M_MPLANE     0x00004000
#define V4L2_CAP_STREAMING            0x04000000
#define V4L2_CAP_DEVICE_CAPS          0x80000000

/* ---------------------------------------------------------------- */
/* /proc helpers                                                    */
/* ---------------------------------------------------------------- */

struct dirent64 { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256]; };

/* read a whole small file into buf (NUL-terminated); returns length or -1 */
static long read_file(const char *path, char *buf, long cap)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0, r;
    while (total < cap-1 && (r = sys_read(fd, buf+total, cap-1-total)) > 0) total += r;
    sys_close(fd);
    buf[total >= 0 ? total : 0] = 0;
    return total;
}

static u32 parse_u32(const char *s){ u32 v=0; while(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++;} return v; }

/* Find pid of a process whose /proc/<pid>/comm == name. Returns pid or 0. */
static long find_pid_by_comm(const char *name)
{
    int fd = sys_open("/proc", O_RDONLY|O_DIRECTORY);
    if (fd < 0) return 0;
    char dbuf[8192];
    long found = 0, n;
    while (!found && (n = sys_getdents64(fd, dbuf, sizeof(dbuf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(dbuf + off);
            const char *nm = d->d_name;
            if (nm[0] >= '0' && nm[0] <= '9') {
                char path[64]; char comm[64];
                /* build /proc/<pid>/comm */
                char *p = path; const char *a = "/proc/";
                while (*a) *p++ = *a++;
                const char *q = nm; while (*q) *p++ = *q++;
                a = "/comm"; while (*a) *p++ = *a++; *p = 0;
                if (read_file(path, comm, sizeof(comm)) > 0) {
                    unsigned L = slen(comm);
                    if (L && comm[L-1] == '\n') comm[L-1] = 0;
                    if (streq(comm, name)) { found = parse_u32(nm); }
                }
            }
            off += d->d_reclen;
        }
    }
    sys_close(fd);
    return found;
}

/* For process <pid>, find an open fd pointing at /dev/video*; returns fd number
 * (as seen inside that process) or -1, and copies the target into out. */
static int find_video_fd(long pid, char *out, long outcap)
{
    char dirp[64]; char *p = dirp; const char *a = "/proc/";
    while (*a) *p++ = *a++;
    /* pid to decimal */
    char num[16]; int i=0; long v=pid; if(!v) num[i++]='0'; while(v){ num[i++]='0'+v%10; v/=10;}
    while(i--) *p++=num[i];
    a="/fd"; while(*a)*p++=*a++; *p=0;

    int fd = sys_open(dirp, O_RDONLY|O_DIRECTORY);
    if (fd < 0) return -1;
    char dbuf[8192]; long n; int result = -1;
    while (result < 0 && (n = sys_getdents64(fd, dbuf, sizeof(dbuf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(dbuf + off);
            const char *nm = d->d_name;
            if (nm[0] >= '0' && nm[0] <= '9') {
                /* readlink /proc/<pid>/fd/<nm> */
                char lp[80]; char *q = lp; const char *b = dirp; while(*b)*q++=*b++; *q++='/';
                const char *r = nm; while(*r)*q++=*r++; *q=0;
                char tgt[256];
                long L = sys_readlink(lp, tgt, sizeof(tgt)-1);
                if (L > 0) { tgt[L]=0;
                    if (sstarts(tgt, "/dev/video")) {
                        result = (int)parse_u32(nm);
                        unsigned k=0; while(tgt[k] && k<outcap-1){ out[k]=tgt[k]; k++; } out[k]=0;
                    }
                }
            }
            off += d->d_reclen;
        }
    }
    sys_close(fd);
    return result;
}

/* ---------------------------------------------------------------- */
/* pretty printers                                                  */
/* ---------------------------------------------------------------- */

static void print_caps(u32 c)
{
    o("    caps:");
    if (c & V4L2_CAP_VIDEO_M2M)            o(" M2M");
    if (c & V4L2_CAP_VIDEO_M2M_MPLANE)     o(" M2M_MPLANE");
    if (c & V4L2_CAP_VIDEO_CAPTURE)        o(" CAPTURE");
    if (c & V4L2_CAP_VIDEO_CAPTURE_MPLANE) o(" CAPTURE_MPLANE");
    if (c & V4L2_CAP_VIDEO_OUTPUT)         o(" OUTPUT");
    if (c & V4L2_CAP_VIDEO_OUTPUT_MPLANE)  o(" OUTPUT_MPLANE");
    if (c & V4L2_CAP_STREAMING)            o(" STREAMING");
    if (c & V4L2_CAP_DEVICE_CAPS)          o(" DEVICE_CAPS");
    onl();
}

static void enum_fmts_on(int fd, u32 type, const char *label)
{
    o("  ENUM_FMT "); o(label); o(":"); onl();
    int any = 0;
    for (u32 idx = 0; idx < 64; idx++) {
        struct v4l2_fmtdesc fd_desc;
        memset(&fd_desc, 0, sizeof(fd_desc));
        fd_desc.index = idx;
        fd_desc.type  = type;
        long r = sys_ioctl(fd, VIDIOC_ENUM_FMT, &fd_desc);
        if (r < 0) break;
        any = 1;
        o("    ["); ou(idx); o("] "); ofourcc(fd_desc.pixelformat);
        o("  "); o((char*)fd_desc.description);
        if (fd_desc.flags & 1) o("  [COMPRESSED]");
        if (fd_desc.flags & 2) o("  [EMULATED]");
        onl();
    }
    if (!any) { o("    (none / ioctl rejected)"); onl(); }
}

static void print_fmt_mp(int fd, u32 type, const char *label)
{
    struct v4l2_format f; memset(&f, 0, sizeof(f)); f.type = type;
    long r = sys_ioctl(fd, VIDIOC_G_FMT, &f);
    o("  G_FMT "); o(label); o(": ");
    if (r < 0) { o("ret="); oi((s32)r); onl(); return; }
    struct v4l2_pix_format_mplane *m = &f.fmt.pix_mp;
    ou(m->width); oc('x'); ou(m->height); o("  ");
    ofourcc(m->pixelformat);
    o("  field="); ou(m->field);
    o("  colorspace="); ou(m->colorspace);
    o("  num_planes="); ou(m->num_planes);
    onl();
    for (int i = 0; i < m->num_planes && i < 8; i++) {
        o("      plane["); ou(i); o("] bytesperline="); ou(m->plane_fmt[i].bytesperline);
        o(" sizeimage="); ou(m->plane_fmt[i].sizeimage); onl();
    }
}

static void print_selection(int fd, u32 type, u32 target, const char *label)
{
    struct v4l2_selection s; memset(&s, 0, sizeof(s)); s.type = type; s.target = target;
    long r = sys_ioctl(fd, VIDIOC_G_SELECTION, &s);
    o("  G_SELECTION "); o(label); o(": ");
    if (r < 0) { o("ret="); oi((s32)r); onl(); return; }
    o("left="); oi(s.r.left); o(" top="); oi(s.r.top);
    o(" "); ou(s.r.width); oc('x'); ou(s.r.height); onl();
}

/* V4L2_SEL_TGT_* */
#define SEL_TGT_CROP        0x0000
#define SEL_TGT_COMPOSE     0x0100

/* ---------------------------------------------------------------- */
/* Layer 1: independent read-only open                              */
/* ---------------------------------------------------------------- */

static void layer1(const char *dev)
{
    o("=== LAYER 1: independent read-only open of "); o(dev); o(" ==="); onl();
    o("(a separate M2M context; does NOT touch muse-server's instance)"); onl();
    int fd = sys_open(dev, O_RDWR);
    if (fd < 0) {
        o("  open() failed ret="); oi((s32)fd);
        o("  (EBUSY=-16 means single-instance decoder already held — expected; "
          "skip to --live)"); onl();
        return;
    }
    struct v4l2_capability cap; memset(&cap, 0, sizeof(cap));
    long r = sys_ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (r < 0) { o("  QUERYCAP ret="); oi((s32)r); onl(); }
    else {
        o("  driver="); o((char*)cap.driver);
        o("  card="); o((char*)cap.card); onl();
        o("  bus="); o((char*)cap.bus_info);
        o("  version="); ox(cap.version); onl();
        o("  capabilities="); ox(cap.capabilities); onl(); print_caps(cap.capabilities);
        if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
            o("  device_caps="); ox(cap.device_caps); onl(); print_caps(cap.device_caps);
        }
    }
    /* coded input side */
    enum_fmts_on(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT_MPLANE (coded in)");
    enum_fmts_on(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,        "OUTPUT (coded in)");
    /* decoded output side — what we care about */
    enum_fmts_on(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE (decoded out)");
    enum_fmts_on(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,        "CAPTURE (decoded out)");
    print_fmt_mp(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE (default)");
    print_fmt_mp(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  "OUTPUT_MPLANE (default)");
    sys_close(fd);
    onl();
}

/* ---------------------------------------------------------------- */
/* Layer 2: live read via ptrace (opt-in, read-only ioctls)         */
/* ---------------------------------------------------------------- */

/* ptrace request numbers */
#define PT_PEEKTEXT   1
#define PT_PEEKDATA   2
#define PT_POKETEXT   4
#define PT_POKEDATA   5
#define PT_CONT       7
#define PT_SINGLESTEP 9
#define PT_GETREGS    12
#define PT_SETREGS    13
#define PT_ATTACH     16
#define PT_DETACH     17

/* arm user_regs: 18 words. [0..12]=r0..r12 [13]=sp [14]=lr [15]=pc [16]=cpsr [17]=orig_r0 */
struct arm_regs { u32 uregs[18]; };
#define R_R0 0
#define R_R1 1
#define R_R2 2
#define R_R7 7
#define R_SP 13
#define R_PC 15
#define R_CPSR 16
#define CPSR_THUMB (1u<<5)

static long pt_peek(long pid, u32 addr){ u32 out=0; long r=sys_ptrace(PT_PEEKDATA, pid, (long)addr, (long)&out); if(r<0) return r; return (long)(u32)out; }
static long pt_poke(long pid, u32 addr, u32 val){ return sys_ptrace(PT_POKEDATA, pid, (long)addr, (long)val); }

/* read block from tracee via word peeks */
static int tracee_read(long pid, u32 addr, void *dst, u32 n)
{
    u8 *d = dst;
    for (u32 i = 0; i < n; i += 4) {
        long w = pt_peek(pid, addr + i);
        if (w < 0) return -1;
        u32 v = (u32)w;
        for (int k = 0; k < 4 && i + k < n; k++) d[i+k] = (v >> (8*k)) & 0xff;
    }
    return 0;
}
static int tracee_write(long pid, u32 addr, const void *src, u32 n)
{
    const u8 *s = src;
    for (u32 i = 0; i < n; i += 4) {
        u32 v;
        if (i + 4 <= n) {
            v = s[i] | (s[i+1]<<8) | (s[i+2]<<16) | (s[i+3]<<24);
        } else {
            long w = pt_peek(pid, addr + i); if (w < 0) return -1; v = (u32)w;
            for (u32 k = 0; i + k < n; k++) v = (v & ~(0xffu<<(8*k))) | ((u32)s[i+k]<<(8*k));
        }
        if (pt_poke(pid, addr + i, v) < 0) return -1;
    }
    return 0;
}

/* find first r-xp mapping base in /proc/<pid>/maps (4-aligned page) */
static u32 find_exec_page(long pid)
{
    char path[48]; char *p = path; const char *a = "/proc/";
    while(*a)*p++=*a++;
    char num[16]; int i=0; long v=pid; if(!v)num[i++]='0'; while(v){num[i++]='0'+v%10;v/=10;}
    while(i--)*p++=num[i];
    a="/maps"; while(*a)*p++=*a++; *p=0;

    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[1024]; long n; char line[256]; int li=0; u32 base=0;
    while (!base && (n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long k=0;k<n;k++){
            char c=buf[k];
            if (c=='\n' || li>=(int)sizeof(line)-1){
                line[li]=0;
                /* parse "START-END PERMS ..." ; want perms containing r-x */
                u32 addr=0; int j=0;
                while (line[j] && line[j]!='-'){ char h=line[j]; u32 d; if(h>='0'&&h<='9')d=h-'0'; else if(h>='a'&&h<='f')d=h-'a'+10; else {d=0;} addr=addr*16+d; j++; }
                while (line[j] && line[j]!=' ') j++;   /* skip -END */
                if (line[j]==' ') j++;
                /* perms at line[j..j+3] */
                if (line[j]=='r' && line[j+2]=='x') { base = addr; }
                li=0;
            } else line[li++]=c;
        }
    }
    sys_close(fd);
    return base;
}

/* inject one read-only ioctl(fd, req, scratch) in the tracee; scratch already
 * populated in tracee mem. Returns ioctl ret (via r0) or <0 on inject failure.
 * Caller saved/poked exec word and will restore regs afterwards. */
static long inject_ioctl(long pid, struct arm_regs *base, u32 injaddr,
                         int tracee_fd, u32 req, u32 scratch)
{
    struct arm_regs r = *base;
    r.uregs[R_R7]   = SYS_ioctl;
    r.uregs[R_R0]   = (u32)tracee_fd;
    r.uregs[R_R1]   = scratch;
    r.uregs[R_R2]   = req;
    r.uregs[R_PC]   = injaddr;
    r.uregs[R_CPSR] = base->uregs[R_CPSR] & ~CPSR_THUMB;   /* force ARM mode */
    if (sys_ptrace(PT_SETREGS, pid, 0, (long)&r) < 0) return -0x1000;
    if (sys_ptrace(PT_SINGLESTEP, pid, 0, 0) < 0) return -0x1001;
    int st; sys_wait4(pid, &st, 0);
    struct arm_regs after; memset(&after,0,sizeof(after));
    if (sys_ptrace(PT_GETREGS, pid, 0, (long)&after) < 0) return -0x1002;
    return (long)(s32)after.uregs[R_R0];
}

static void layer2(int force_dev)
{
    o("=== LAYER 2: LIVE read of muse-server's decoder fd via ptrace ==="); onl();
    o("(read-only ioctls injected into muse-server, then regs/mem restored)"); onl();

    long pid = find_pid_by_comm("muse-server");
    if (!pid) { o("  muse-server not found (not playing?). Aborted, nothing touched."); onl(); return; }
    o("  muse-server pid="); ou((u32)pid); onl();

    char dev[256];
    int tfd = find_video_fd(pid, dev, sizeof(dev));
    if (tfd < 0) { o("  muse-server holds no /dev/video* fd. Aborted."); onl(); return; }
    o("  decoder fd (inside muse-server) = "); ou((u32)tfd); o("  -> "); o(dev); onl();
    o("  VIDIOC_G_FMT number for this target = "); ox((u32)VIDIOC_G_FMT); onl();

    u32 injaddr = find_exec_page(pid);
    if (!injaddr) { o("  no r-xp page found in maps. Aborted (no injection done)."); onl(); return; }
    o("  inject addr (r-xp page base) = "); ox(injaddr); onl();

    if (sys_ptrace(PT_ATTACH, pid, 0, 0) < 0) {
        o("  PTRACE_ATTACH failed (Smack/yama?). Aborted, nothing touched."); onl(); return;
    }
    int st; sys_wait4(pid, &st, 0);

    /* save regs + the exec word we overwrite */
    struct arm_regs saved; memset(&saved,0,sizeof(saved));
    long gr = sys_ptrace(PT_GETREGS, pid, 0, (long)&saved);
    long origword = pt_peek(pid, injaddr);
    int poked = 0;

    if (gr < 0 || origword < 0) {
        o("  GETREGS/PEEK failed; detaching without injection."); onl();
        goto detach;
    }

    /* scratch buffer in tracee stack, well below sp */
    u32 scratch = (saved.uregs[R_SP] - 2048) & ~7u;
    u8 savebytes[256];
    if (tracee_read(pid, scratch, savebytes, sizeof(savebytes)) < 0) {
        o("  could not read scratch region; detaching without injection."); onl();
        goto detach;
    }

    /* place ARM 'svc 0' (0xef000000) at injaddr */
    if (pt_poke(pid, injaddr, 0xef000000u) < 0) {
        o("  could not poke svc; detaching without injection."); onl();
        goto detach;
    }
    poked = 1;

    /* --- read-only query 1: G_FMT CAPTURE_MPLANE (decoded output) --- */
    {
        struct v4l2_format f; memset(&f,0,sizeof(f)); f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        tracee_write(pid, scratch, &f, sizeof(f));
        long rc = inject_ioctl(pid, &saved, injaddr, tfd, (u32)VIDIOC_G_FMT, scratch);
        if (rc <= -0x1000) { o("  inject error "); ox((u32)(-rc)); onl(); }
        else {
            tracee_read(pid, scratch, &f, sizeof(f));
            o("  LIVE G_FMT CAPTURE_MPLANE: ret="); oi((s32)rc); o("  ");
            if (rc == 0) {
                struct v4l2_pix_format_mplane *m=&f.fmt.pix_mp;
                ou(m->width); oc('x'); ou(m->height); o("  "); ofourcc(m->pixelformat);
                o("  num_planes="); ou(m->num_planes); o("  colorspace="); ou(m->colorspace); onl();
                for(int i=0;i<m->num_planes&&i<8;i++){ o("      plane["); ou(i); o("] bpl=");
                    ou(m->plane_fmt[i].bytesperline); o(" sizeimage="); ou(m->plane_fmt[i].sizeimage); onl(); }
            } else onl();
        }
    }
    /* --- read-only query 2: G_FMT OUTPUT_MPLANE (coded input) --- */
    {
        struct v4l2_format f; memset(&f,0,sizeof(f)); f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        tracee_write(pid, scratch, &f, sizeof(f));
        long rc = inject_ioctl(pid, &saved, injaddr, tfd, (u32)VIDIOC_G_FMT, scratch);
        tracee_read(pid, scratch, &f, sizeof(f));
        o("  LIVE G_FMT OUTPUT_MPLANE: ret="); oi((s32)rc); o("  ");
        if (rc == 0){ struct v4l2_pix_format_mplane *m=&f.fmt.pix_mp;
            ou(m->width); oc('x'); ou(m->height); o("  "); ofourcc(m->pixelformat); onl(); } else onl();
    }
    /* --- read-only query 3: G_SELECTION compose (display rect) --- */
    {
        struct v4l2_selection s; memset(&s,0,sizeof(s));
        s.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; s.target = SEL_TGT_COMPOSE;
        tracee_write(pid, scratch, &s, sizeof(s));
        long rc = inject_ioctl(pid, &saved, injaddr, tfd, (u32)VIDIOC_G_SELECTION, scratch);
        tracee_read(pid, scratch, &s, sizeof(s));
        o("  LIVE G_SELECTION compose: ret="); oi((s32)rc); o("  ");
        if (rc == 0){ o("left="); oi(s.r.left); o(" top="); oi(s.r.top); o(" ");
            ou(s.r.width); oc('x'); ou(s.r.height); onl(); } else onl();
    }

    /* restore the scratch bytes */
    tracee_write(pid, scratch, savebytes, sizeof(savebytes));

detach:
    if (poked) pt_poke(pid, injaddr, (u32)origword);
    if (gr >= 0) sys_ptrace(PT_SETREGS, pid, 0, (long)&saved);
    sys_ptrace(PT_DETACH, pid, 0, 0);
    o("  detached; muse-server registers and memory restored."); onl();
    onl();
}

/* ---------------------------------------------------------------- */
/* Layer 0 + driver                                                 */
/* ---------------------------------------------------------------- */

static void dump_small(const char *path, const char *label)
{
    char b[4096];
    long r = read_file(path, b, sizeof(b));
    o("  "); o(label); o(": ");
    if (r < 0) { o("(unreadable)"); onl(); return; }
    /* trim trailing newline */
    if (r>0 && b[r-1]=='\n') b[r-1]=0;
    o(b); onl();
}

static void layer0(char *dev_out, long devcap)
{
    o("=== LAYER 0: passive discovery (/proc + /sys, muse-server untouched) ==="); onl();
    long pid = find_pid_by_comm("muse-server");
    if (!pid) { o("  muse-server not running (start playback first for live data)."); onl(); }
    else {
        o("  muse-server pid="); ou((u32)pid); onl();
        char dev[256];
        int tfd = find_video_fd(pid, dev, sizeof(dev));
        if (tfd >= 0) {
            o("  decoder fd="); ou((u32)tfd); o("  -> "); o(dev); onl();
            /* copy device name out for layer1 default */
            unsigned k=0; while(dev[k]&&k<devcap-1){ dev_out[k]=dev[k]; k++; } dev_out[k]=0;
            /* sysfs name for that videoN */
            const char *base = dev; const char *nm = base;
            for (const char *s=base; *s; s++) if (*s=='/') nm=s+1;   /* "video61" */
            char sp[96]; char *p=sp; const char *a="/sys/class/video4linux/";
            while(*a)*p++=*a++; const char *q=nm; while(*q)*p++=*q++;
            char full[128];
            /* name */
            { char *e=full; const char*s=sp; while(*s)*e++=*s++; const char*t="/name"; while(*t)*e++=*t++; *e=0; }
            dump_small(full, "sysfs name");
        } else { o("  muse-server holds no /dev/video* fd right now."); onl(); }
    }
    onl();
}

int main(int argc, char **argv)
{
    int want_live = 0; const char *force_dev = 0;
    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--live")) want_live = 1;
        else if (streq(argv[i], "--dev") && i+1 < argc) force_dev = argv[++i];
    }

    o("nvdec-probe (read-only V4L2 decoder inspection) — armv7"); onl();
    o("VIDIOC_QUERYCAP="); ox((u32)VIDIOC_QUERYCAP);
    o(" ENUM_FMT="); ox((u32)VIDIOC_ENUM_FMT);
    o(" G_FMT="); ox((u32)VIDIOC_G_FMT);
    o(" G_SELECTION="); ox((u32)VIDIOC_G_SELECTION); onl();
    o("sizeof v4l2_format="); ou((u32)sizeof(struct v4l2_format));
    o(" fmtdesc="); ou((u32)sizeof(struct v4l2_fmtdesc));
    o(" cap="); ou((u32)sizeof(struct v4l2_capability)); onl(); onl();

    char dev[256]; dev[0]=0;
    layer0(dev, sizeof(dev));

    const char *d1 = force_dev ? force_dev : (dev[0] ? dev : "/dev/video61");
    layer1(d1);

    if (want_live) layer2(0);
    else { o("(Layer 2 live-read skipped; pass --live to read muse-server's active fd)"); onl(); }

    flush();
    return 0;
}

/* freestanding entry: r0 := sp (points at argc), then exit_group(main_ret) */
__asm__(
".global _start\n"
"_start:\n"
"  mov r0, sp\n"
"  bl  _c_start\n"
"  mov r7, #248\n"   /* exit_group */
"  svc 0\n"
);

int _c_start(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    return main(argc, argv);
}
