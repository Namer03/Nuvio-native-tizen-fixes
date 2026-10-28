// NvProbe — read-only V4L2 inspection of the TV's video decoder, as managed
// code so it runs under the TV's coreclr (UEP cannot block a JITed IL assembly
// the way it blocks a native ELF). Launched as root via vetro:
//     /usr/bin/dotnet NvProbe.dll [/dev/videoN]
//
// Read-only: issues only V4L2 *get* ioctls (QUERYCAP / ENUM_FMT / G_FMT /
// G_SELECTION). Never sets a format, allocates buffers, or streams.
//
// This opens its OWN handle to the decoder node (a separate M2M context). If
// the decoder is single-instance and muse-server already holds it, open()
// returns EBUSY (-16) — that result tells us we must escalate to a ptrace read
// of muse-server's live fd instead.
//
// ioctl numbers are the armv7 (32-bit kernel) values, verified against the
// struct sizes on that ABI (e.g. sizeof(v4l2_format)=204 -> G_FMT=0xc0cc5604).

using System;
using System.Runtime.InteropServices;
using System.Text;

static class NvProbe
{
    [DllImport("libc", SetLastError = true)]
    static extern int open(string path, int flags);
    [DllImport("libc", SetLastError = true)]
    static extern int close(int fd);
    [DllImport("libc", SetLastError = true)]
    static extern int ioctl(int fd, uint request, [In, Out] byte[] arg);

    const int O_RDWR = 2;

    // armv7 V4L2 ioctl numbers
    const uint VIDIOC_QUERYCAP    = 0x80685600;
    const uint VIDIOC_ENUM_FMT    = 0xc0405602;
    const uint VIDIOC_G_FMT       = 0xc0cc5604; // sizeof v4l2_format = 204 on arm32
    const uint VIDIOC_G_SELECTION = 0xc0405619; // nr=25, size 64

    const uint BUF_CAPTURE        = 1;
    const uint BUF_OUTPUT         = 2;
    const uint BUF_CAPTURE_MPLANE = 9;
    const uint BUF_OUTPUT_MPLANE  = 10;

    const uint SEL_TGT_CROP    = 0x0000;
    const uint SEL_TGT_COMPOSE = 0x0100;

    static uint U32(byte[] b, int off) =>
        (uint)(b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24));
    static int I32(byte[] b, int off) => unchecked((int)U32(b, off));
    static void PutU32(byte[] b, int off, uint v)
    { b[off]=(byte)v; b[off+1]=(byte)(v>>8); b[off+2]=(byte)(v>>16); b[off+3]=(byte)(v>>24); }

    static string FourCC(uint f)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < 4; i++) { char c=(char)((f>>(8*i))&0xff); sb.Append(c>=32&&c<127?c:'.'); }
        return $"{sb} (0x{f:x8})";
    }
    static string CStr(byte[] b, int off, int max)
    {
        int n=0; while (n<max && b[off+n]!=0) n++;
        return Encoding.ASCII.GetString(b, off, n);
    }
    static int Errno() => Marshal.GetLastWin32Error();

    static void Main(string[] args)
    {
        string dev = args.Length > 0 ? args[0] : "/dev/video61";
        Console.WriteLine("NvProbe (managed, read-only V4L2) — dev=" + dev);
        Console.WriteLine($"ioctl#: QUERYCAP=0x{VIDIOC_QUERYCAP:x8} ENUM_FMT=0x{VIDIOC_ENUM_FMT:x8} " +
                          $"G_FMT=0x{VIDIOC_G_FMT:x8} G_SELECTION=0x{VIDIOC_G_SELECTION:x8}");

        int fd = open(dev, O_RDWR);
        if (fd < 0)
        {
            int e = Errno();
            Console.WriteLine($"open({dev}) failed errno={e}" +
                (e==16 ? "  (EBUSY: single-instance decoder already held by muse-server — need the ptrace/live path)" :
                 e==13 ? "  (EACCES: not permitted even as root?)" : ""));
            return;
        }
        Console.WriteLine($"open ok fd={fd}");

        QueryCap(fd);
        EnumFmt(fd, BUF_OUTPUT_MPLANE,  "OUTPUT_MPLANE (coded in)");
        EnumFmt(fd, BUF_OUTPUT,         "OUTPUT (coded in)");
        EnumFmt(fd, BUF_CAPTURE_MPLANE, "CAPTURE_MPLANE (decoded out)");
        EnumFmt(fd, BUF_CAPTURE,        "CAPTURE (decoded out)");
        GFmtMp(fd, BUF_CAPTURE_MPLANE,  "CAPTURE_MPLANE (default)");
        GFmtMp(fd, BUF_OUTPUT_MPLANE,   "OUTPUT_MPLANE (default)");
        GSel(fd, BUF_CAPTURE_MPLANE, SEL_TGT_COMPOSE, "CAPTURE compose");
        GSel(fd, BUF_OUTPUT_MPLANE,  SEL_TGT_CROP,    "OUTPUT crop");

        close(fd);
        Console.WriteLine("done.");
    }

    static void QueryCap(int fd)
    {
        var b = new byte[104];
        int r = ioctl(fd, VIDIOC_QUERYCAP, b);
        if (r < 0) { Console.WriteLine($"QUERYCAP errno={Errno()}"); return; }
        uint caps = U32(b, 84), dcaps = U32(b, 88);
        Console.WriteLine($"QUERYCAP driver='{CStr(b,0,16)}' card='{CStr(b,16,32)}' bus='{CStr(b,48,32)}'");
        Console.WriteLine($"  capabilities=0x{caps:x8}{CapStr(caps)}");
        if ((caps & 0x80000000) != 0)
            Console.WriteLine($"  device_caps =0x{dcaps:x8}{CapStr(dcaps)}");
    }
    static string CapStr(uint c)
    {
        var s = new StringBuilder("  [");
        if ((c&0x00008000)!=0) s.Append("M2M ");
        if ((c&0x00004000)!=0) s.Append("M2M_MPLANE ");
        if ((c&0x00001000)!=0) s.Append("CAP_MPLANE ");
        if ((c&0x00002000)!=0) s.Append("OUT_MPLANE ");
        if ((c&0x00000001)!=0) s.Append("CAP ");
        if ((c&0x00000002)!=0) s.Append("OUT ");
        if ((c&0x04000000)!=0) s.Append("STREAMING ");
        return s.Append("]").ToString();
    }

    static void EnumFmt(int fd, uint type, string label)
    {
        Console.WriteLine($"ENUM_FMT {label}:");
        bool any = false;
        for (uint i = 0; i < 64; i++)
        {
            var b = new byte[64];
            PutU32(b, 0, i);      // index
            PutU32(b, 4, type);   // type
            if (ioctl(fd, VIDIOC_ENUM_FMT, b) < 0) break;
            any = true;
            uint flags = U32(b, 8);
            string desc = CStr(b, 12, 32);
            uint pf = U32(b, 44);
            string fl = ((flags&1)!=0?" COMPRESSED":"") + ((flags&2)!=0?" EMULATED":"");
            Console.WriteLine($"  [{i}] {FourCC(pf)}  {desc}{fl}");
        }
        if (!any) Console.WriteLine("  (none / rejected)");
    }

    static void GFmtMp(int fd, uint type, string label)
    {
        var b = new byte[204];
        PutU32(b, 0, type);
        int r = ioctl(fd, VIDIOC_G_FMT, b);
        if (r < 0) { Console.WriteLine($"G_FMT {label}: errno={Errno()}"); return; }
        uint w = U32(b,4), h = U32(b,8), pf = U32(b,12), field = U32(b,16), cs = U32(b,20);
        uint np = b[184];
        Console.WriteLine($"G_FMT {label}: {w}x{h}  {FourCC(pf)}  field={field} colorspace={cs} num_planes={np}");
        for (int p = 0; p < np && p < 8; p++)
        {
            int baseoff = 24 + p*20;
            Console.WriteLine($"    plane[{p}] sizeimage={U32(b,baseoff)} bytesperline={U32(b,baseoff+4)}");
        }
    }

    static void GSel(int fd, uint type, uint target, string label)
    {
        var b = new byte[64];
        PutU32(b, 0, type);
        PutU32(b, 4, target);
        int r = ioctl(fd, VIDIOC_G_SELECTION, b);
        if (r < 0) { Console.WriteLine($"G_SELECTION {label}: errno={Errno()}"); return; }
        Console.WriteLine($"G_SELECTION {label}: left={I32(b,12)} top={I32(b,16)} {U32(b,20)}x{U32(b,24)}");
    }
}
