# muse-probe — read-only inspection of muse-server's video decoder

Phase 0 follow-up for the libass "video lane" work
(`claude/libass-video-lane-PLAN.md`). The S95C has no `v4l2-ctl`, `strace` or
`gdb`, so this is the small helper the plan calls for to answer, **without
changing anything on the TV**:

- what pixel format / size the decoder (`/dev/video61`, `sdp-dvde-dec1`) emits,
- what formats it can emit (capability enumeration),
- the live, negotiated format for the stream that's actually playing.

Everything it does is a V4L2 **get** ioctl (`QUERYCAP`, `ENUM_FMT`, `G_FMT`,
`G_SELECTION`). It never sets a format, allocates/queues buffers, starts/stops
streaming, or writes to any device. Nothing persists across a reboot.

## Three layers (least intrusive first)

| Layer | What | Touches muse-server? | Needs playback? |
|------|------|----------------------|-----------------|
| 0 | Passive discovery via `/proc` + `/sys`: find muse-server, confirm its `/dev/videoN` fd, read the sysfs name. | No | No (shows live fd if yes) |
| 1 | Open a **separate** handle to the same `/dev/videoN` (a new M2M context) and read `QUERYCAP` + `ENUM_FMT` (OUTPUT=coded, CAPTURE=decoded) + default `G_FMT`. | No — independent instance | No |
| 2 (`--live`) | `ptrace`-attach muse-server and inject **read-only** `G_FMT`/`G_SELECTION` on its real decoder fd, then restore registers+memory and detach. | Yes — momentarily stops its main thread, like a debugger | **Yes** |

Layer 2 is opt-in. It is read-only with respect to the decoder, but it does
briefly stop muse-server. `muse-server.service` is `Restart=always`, so the
worst realistic failure is a brief A/V glitch, never a permanent change. The
code validates every precondition (attach, GETREGS, an `r-xp` page, scratch
read) and detaches **without injecting** if anything is off, and always
restores registers + the one patched word before detaching.

## Build

```sh
./build.sh                 # -> ./nvdec-probe  (static armv7 EABI, no libc)
```

Needs only `clang` + `lld` on the build host — no ARM sysroot. It's built
freestanding (raw syscalls, no libc) so it runs on the TV regardless of Tizen's
userland. The V4L2 ioctl numbers are computed by the compiler from the struct
definitions **for the armv7 target**, so they match the TV's 32-bit kernel
(e.g. `VIDIOC_G_FMT = 0xc0cc5604`, from `sizeof(v4l2_format) == 204`, not the
x86 `0xc0d05604`).

## Run on the TV (as root, via vetro `/exec` + the nuvio-root runner)

The `scripts/` files are ready to drop into the runner's `queue\`:

1. `20-nvdec-deploy.sh` — base64-decodes the binary to `/tmp/nvdec-probe` and
   runs Layers 0+1. Safe any time; run it first. (Play an episode first if you
   also want Layer 0 to show the live decoder fd.)
2. `21-nvdec-live.sh` — runs `--live` (Layer 2). **Only while an episode is
   playing.** Requires `20-` to have deployed the binary (it lives in `/tmp`,
   which is tmpfs, until reboot).

Direct invocation if you copy the binary over yourself:

```sh
/tmp/nvdec-probe            # Layers 0 + 1
/tmp/nvdec-probe --live     # + Layer 2 (playback active)
/tmp/nvdec-probe --dev /dev/video61   # force the device for Layer 1
```

## Reading the output

- **Layer 1 `ENUM_FMT CAPTURE_*`** lists the fourccs the decoder can output
  (e.g. `NV12`, a Samsung tiled variant, …) — this is the key input to the
  Phase 1 blend benchmark.
- **Layer 1 `G_FMT CAPTURE_MPLANE (default)`** gives a default geometry /
  `bytesperline` / `sizeimage`; the live values come from Layer 2.
- **Layer 2 `LIVE G_FMT CAPTURE_MPLANE`** is the actually-negotiated decoded
  size + format + plane strides for the playing stream.
- An `EBUSY` (`-16`) on the Layer 1 open means the decoder is single-instance
  and already held by muse-server — expected; use `--live` for that case.

Still **not** answered by this tool (next Phase 0 step): whether those decoder
buffers can be mapped via `/dev/sdp_hwmem`. That needs buffer handles, which
only make sense once the format/memory model here is known.
