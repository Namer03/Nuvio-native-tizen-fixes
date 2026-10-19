# Nuvio Native Legacy (Tizen Fixes Fork)

This is a custom fork of [iqui27/nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy), an unofficial native TV client for LG webOS, Samsung Tizen, and Android TV built using C, SDL2, and OpenGL ES 2.0.

This fork focuses on stability fixes and new features for **Samsung Tizen devices**, with a particular focus on Tizen 6 and Tizen 9. Testing has been performed on Samsung S95C hardware.

> ⚠️ **Development Status: Active Testing and Setup**
>
> Application changes are being integrated into the main application. Live feature validation and runtime testing are pending local Nuvio Supabase backend key configuration.

---

## 🛠️ Main Application Fixes

- **Regex Engine Stability:** Core regular-expression stability fixes have been integrated into the main application logic. **Status:** Pushed.
- **Microphone Voice Search:** Adds microphone input capabilities for voice-activated content searches directly within the Tizen interface. **Status:** Pushed.

## ✨ Features and Roadmap

- **DTS and Dolby TrueHD Downmixing:** Implements audio downmixing to convert high-definition audio tracks into compatible stereo formats, helping prevent silent playback and audio-related playback failures on native TV speakers. **Status:** Main application integration and debugging active.
- **Source Management and Tab Overhaul:** Plans to rework the "All" tab layout and implement robust source sorting mechanisms. **Status:** Next feature / Planned.

---

## 🚀 Upstream Project Context

This fork inherits the core capabilities of the upstream `nuvio-native-legacy` client:

- **High Performance:** Native C implementation designed for high frame rates and low memory overhead on TV hardware.
- **Multi-Platform Engine:** Native Tizen package support across supported device generations, using OpenGL ES without an underlying web engine.
- **Feature Rich:** Integrations with Trakt, Simkl, debrid services, and custom ASS/SSA subtitle rendering.

For the full architecture, compilation toolchains, and original implementation guidelines, see the [upstream README](https://github.com/iqui27/nuvio-native-legacy#readme).

---

## 🛠️ Building and Development

### Configuration

Configuration secrets, including Supabase backend credentials, are excluded from the source repository. Builds read configuration from a local `local.properties` file through `tools/env.sh`.

Example configuration:

```properties
NUVIO_SUPABASE_URL=...
NUVIO_SUPABASE_ANON_KEY=...
TV_LOGIN_WEB_BASE_URL=...
```

Keep your actual credentials private. Do not commit `local.properties` to the repository.

### Build on GitHub (No Local Setup)

The workflow `.github/workflows/build-tpk.yml` builds the `NuvioTpk` package for Tizen 8+/9 on GitHub Actions. It runs on pushes and can also be triggered manually.

1. **Configure the build secrets:** Open **Settings → Secrets and variables → Actions → New repository secret**. Create a secret named `LOCAL_PROPERTIES` and paste in the contents of your `local.properties` configuration. Without this secret, the build can still run, but API-dependent features may not work.

2. **Start a build:** Push to a branch containing the workflow, or open **Actions → Build TPK → Run workflow** and select the desired branch. The manual workflow option becomes available once the workflow is present on `master`. Older branches may need to merge `master` to obtain the workflow file.

3. **Download the package:** Open the completed workflow run, find the **Artifacts** section, and download `NuvioTpk`. Extract the archive to obtain the `.tpk` package. Artifacts are retained for 90 days.

**Build time:** The first build may take approximately 1–2 hours because native C dependencies are compiled under ARM emulation. Subsequent builds can be faster because cached libraries are reused. If a build fails, the workflow displays the last 60 lines of the log near the top of the run page.

> **Security note:** Anyone with access to a public repository's workflow artifacts may be able to download the resulting package. If credentials are compiled into the `.tpk`, they may be recoverable from the package. Use a private repository when access must be restricted, and avoid embedding sensitive secrets in distributed binaries.

### Build Locally

Requirements depend on the target package and build script. The commands below reflect the existing project scripts.

```bash
# Build the native Samsung Tizen .tpk targets.
# Requires Docker and the .NET 8 SDK.
bash tools/tpk.sh

# Build only the Tizen 8+/9 package.
# Requires the Samsung .NET workload.
NV_TPK_PACOTES=NuvioTpk bash tools/tpk.sh

# Build and check all native Samsung packages for a release.
bash tools/release-samsung.sh
```

#### Docker ARM Platform Compatibility

On systems where Docker rejects the `linux/arm/v5` platform name, including the documented GitHub Actions environment, the project notes recommend using `linux/arm` instead.

To apply that change to `tools/tpk.sh`:

```bash
sed -i 's#linux/arm/v5#linux/arm#g' tools/tpk.sh
```

Review the script before applying this modification, especially if you maintain other build targets.

#### Application Artwork

Converting application artwork requires `ffmpeg` or, on macOS, `sips` on the build machine.

#### Package Signing and Deployment

> **Note:** Packages are intentionally produced unsigned. Before deploying a generated `.tpk` to physical Samsung TV hardware, sign it using an appropriate tool, such as Apps2Samsung, with a compatible device DUID profile.

---

## 📜 License and Credits

- **Original Project:** Credit for the underlying native client engine goes to `iqui27` and the CraaazyDevs community.
- **License:** This fork is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
- **Branding:** The Nuvio name and logos belong to their respective trademark owners.

For upstream project documentation and original development guidance, visit the [Nuvio Native Legacy repository](https://github.com/iqui27/nuvio-native-legacy).
sources, every artwork source, memory, frame times), picks the best settings
for it, checks before and after, and keeps the new profile only if it's clearly
better than network noise.

Then it sends an anonymous report: timings and sizes. **No passwords, tokens,
addon URLs or personal lists.** Each report tells me the right memory budget,
network threads and image sizes for another TV model.

---

## Support development

<a href="https://www.patreon.com/cw/CraaazyDevs">
  <picture>
    <source media="(max-width: 640px)" srcset="docs/assets/patreon-support-banner-mobile.png">
    <img src="docs/assets/patreon-support-banner.png" alt="Support Nuvio Native Legacy by CraaazyDevs on Patreon. Built with care. Supported by the community. Support is optional." width="1600">
  </picture>
</a>

This project grew out of wanting a smooth, beautiful Nuvio experience on my LG C9. It now reaches more TVs, with help from people testing builds and sharing feedback.

If you’d like to support the time and care behind it, visit [CraaazyDevs on Patreon](https://www.patreon.com/cw/CraaazyDevs). Support is entirely optional. Bug reports, testing, and helping other users matter too.

This is an independent, unofficial project, not affiliated with NuvioMedia. Credit for Nuvio goes to its original creators.

---

## Reporting a problem

1. Right after the problem happens, send the log from the app: **Settings ›
   About › Send log** (on LG you can also open the log panel from the remote).
2. Open an [issue](https://github.com/iqui27/nuvio-native-legacy/issues) with your
   TV model, the app version and what you did.

The log never carries credentials. Logs are kept for 30 days and are only used
to fix bugs. You can also turn on automatic log sending in Settings.

---

## Building

```bash
bash tools/mac.sh              # build and run on macOS (UI only, no video)
bash tools/linux.sh            # build and run on Linux (UI only, no video)
bash tools/linux.sh --build    # compile the Linux desktop preview only
bash tools/linux.sh --preview  # browse the Linux UI without signing in
bash tools/arm.sh              # cross-compile for LG in Docker and deploy over ssh
bash tools/arm.sh --ipk        # also produce the .ipk
bash tools/tizen.sh            # build the Samsung target (WebAssembly)
bash tools/tizen-wgt.sh        # package it as an unsigned .wgt
bash tools/tpk.sh              # native Samsung: the four .tpk (Docker + .NET 8)
bash tools/release-samsung.sh  # every Samsung package for a release, checked
bash tools/hb-repo.sh <ipk> <dir>   # Homebrew Channel repo files for a release
```

On an x86_64 Linux host with Podman, build the SDK with its x86_64 toolchain
and select the runtime and packaging CLI explicitly:

```bash
podman build --platform linux/amd64 \
  --build-arg SDK_URL=https://github.com/openlgtv/buildroot-nc4/releases/download/webos-a38c582/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz \
  -t nuvio-webos-sdk tools/
NUVIO_CONTAINER_RUNTIME=podman NUVIO_BUILD_PLATFORM=linux/amd64 \
  NUVIO_ARES_PACKAGE=/path/to/ares-package bash tools/arm.sh --ipk --build
```

`--ipk --build` creates the package without deploying to a TV. The target
binary is ARMv7 regardless of the build host architecture.

webOS builds include DTS audio fallback by default. TVs that accept DTS keep
native playback; when the selected DTS track is unsupported, the app converts
its audio to stereo AAC and keeps video on the TV's hardware pipeline. See the
[DTS guide](docs/features/dts/README.md) for limits and verification, and
[DTS Debug build commands](tools/dts-pipeline/DEBUG.md) for a separate diagnostic
app that can be installed alongside the normal one.

For Linux UI previews, install the SDL2, SDL2_image, SDL2_ttf, GLES2, EGL and
zlib development packages, a C compiler and pkg-config. `--preview` skips login
and uses a separate data directory. Arrow keys and Enter navigate; Escape or
Backspace goes back. Video playox torrents can be sent to download from the Sources sheet
- Automatic source: **Best source** or **First in list** (like Nuvio's auto-play
  first source), checked one at a time, with a limited number of retries
- **ASS/SSA subtitles** embedded in MKV (anime) drawn by the app with libass, read
  in the background over HTTP ranges while the video plays
- Addon subtitles, preferred subtitle/audio language that turns on by itself,
  subtitle style
- Up Next / More like this at the credits (MKV chapters and TheIntroDB), skip
  intro, resume
- Seek shows only the progress bar when the controls are hidden

**Also**
- Live TV (Xtream and Stalker portals) with a guide
- Blur unwatched episodes (spoiler protection)
- Recommend a title to a friend
- A **Diagnostic** that measures your TV and tunes the app for it (below)

What it doesn't do yet: Nuvio **plugins** (JavaScript scrapers). This app runs
Stremio addons, but it has no JavaScript engine for plugins (#134).

---

## The Diagnostic, and why I ask you to run it

Almost everything that made this app faster came from real logs, and I only own
one TV. So there's a **Diagnostic** in **Settings › Diagnostics › Diagnostics
and optimization**. It takes about a minute. It measures your TV (addons,
sources, every artwork source, memory, frame times), picks the best settings
for it, checks before and after, and keeps the new profile only if it's clearly
better than network noise.

Then it sends an anonymous report: timings and sizes. **No passwords, tokens,
addon URLs or personal lists.** Each report tells me the right memory budget,
network threads and image sizes for another TV model.

---

## Support development

<a href="https://www.patreon.com/cw/CraaazyDevs">
  <picture>
    <source media="(max-width: 640px)" srcset="docs/assets/patreon-support-banner-mobile.png">
    <img src="docs/assets/patreon-support-banner.png" alt="Support Nuvio Native Legacy by CraaazyDevs on Patreon. Built with care. Supported by the community. Support is optional." width="1600">
  </picture>
</a>

This project grew out of wanting a smooth, beautiful Nuvio experience on my LG C9. It now reaches more TVs, with help from people testing builds and sharing feedback.

If you’d like to support the time and care behind it, visit [CraaazyDevs on Patreon](https://www.patreon.com/cw/CraaazyDevs). Support is entirely optional. Bug reports, testing, and helping other users matter too.

This is an independent, unofficial project, not affiliated with NuvioMedia. Credit for Nuvio goes to its original creators.

---

## Reporting a problem

### Troubleshoot first

Most playback reports turn out to be the add-on, the debrid service or the
connection, not this app. One minute of checks saves everyone time:

1. **Play the same title with the same source in another player** (Stremio, the
   official Nuvio app, VLC, Kodi). If it also fails there, ask the **add-on
   author** or your **debrid's support** — nothing can be fixed here.
2. **"Not cached", no sources, 404 or an expired link** are always add-on or
   debrid side.
3. **Try another source, then another title**, and run the internet speed test
   on the TV: **Settings › This TV › Speed test** reports the
   largest file your connection can handle. If the file you are playing is bigger
   than that, the connection is the limit — pick a smaller source, that is not a
   bug in this app.
4. **Send the log and keep its code**: **Settings › About and help › Send
   log › Send**. When it finishes, the screen shows a **6-character code**
   (letters and digits, no I, L, O or U). That code is what we need in the issue.

The log never carries credentials. Logs are kept for 30 days and are only used
to fix bugs. You can also turn on automatic log sending in Settings.

### File it

1. Right after the problem happens, send the log (step 5 above).
2. Open an [issue](https://github.com/iqui27/nuvio-native-legacy/issues) and fill
   the **Bug report** form: platform, TV model and system version, app version,
   the 6-character log code, and what you did.

---

## Building

```bash
bash tools/mac.sh              # build and run on macOS (UI only, no video)
bash tools/linux.sh            # build and run on Linux (UI only, no video)
bash tools/linux.sh --build    # compile the Linux desktop preview only
bash tools/linux.sh --preview  # browse the Linux UI without signing in
bash tools/arm.sh              # cross-compile for LG in Docker and deploy over ssh
bash tools/arm.sh --ipk        # also produce the .ipk
bash tools/tizen.sh            # build the Samsung target (WebAssembly)
bash tools/tizen-wgt.sh        # package it as an unsigned .wgt
bash tools/tpk.sh              # native Samsung: the four .tpk (Docker + .NET 8)
bash tools/release-samsung.sh  # every Samsung package for a release, checked
bash tools/hb-repo.sh <ipk> <dir>   # Homebrew Channel repo files for a release
```

On an x86_64 Linux host with Podman, build the SDK with its x86_64 toolchain
and select the runtime and packaging CLI explicitly:

```bash
podman build --platform linux/amd64 \
  --build-arg SDK_URL=https://github.com/openlgtv/buildroot-nc4/releases/download/webos-a38c582/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz \
  -t nuvio-webos-sdk tools/
NUVIO_CONTAINER_RUNTIME=podman NUVIO_BUILD_PLATFORM=linux/amd64 \
  NUVIO_ARES_PACKAGE=/path/to/ares-package bash tools/arm.sh --ipk --build
```

`--ipk --build` creates the package without deploying to a TV. The target
binary is ARMv7 regardless of the build host architecture.

webOS builds include DTS audio fallback by default. TVs that accept DTS keep
native playback; when the selected DTS track is unsupported, the app converts
its audio to stereo AAC and keeps video on the TV's hardware pipeline. See the
[DTS guide](docs/features/dts/README.md) for limits and verification, and
[DTS Debug build commands](tools/dts-pipeline/DEBUG.md) for a separate diagnostic
app that can be installed alongside the normal one.

For Linux UI previews, install the SDL2, SDL2_image, SDL2_ttf, GLES2, EGL and
zlib development packages, a C compiler and pkg-config. `--preview` skips login
and uses a separate data directory. Arrow keys and Enter navigate; Escape or
Backspace goes back. Video playback is unavailable in this desktop preview.

Builds read `local.properties` from the repository root or the legacy neighboring
web project; `NUVIO_PROPERTIES` selects another file.

Tests are shell scripts in `tests/` (`bash tests/<name>.sh`); most of them build
a piece of `src/` on the host with the network or the TV faked.

Server URLs and client ids are **not in the source**. They come from a
`local.properties` file through `tools/env.sh`. The build refuses to package any
credential file and deletes the `.ipk` if one shows up (`tools/testa-ipk.sh`
checks by extracting the `ar` archive).

---

## Things I learned porting to webOS

Each one of these cost me at least a day:

- The video plane sits **behind** the GL surface and shows through an alpha
  hole. `glReadPixels` and the TV's screenshot never see it, so a capture during
  playback is black. That's the design, not a bug.
- `StarfishMediaAPIs` calls `exit(0)` when the process doesn't match `exeName` in
  its LS2 role. Silent, no crash. Talking to `com.webos.media` over LS2 directly
  is what works.
- webOS 5 removed `libAcbAPI`. The app carries both paths: ACB on webOS 2–4,
  SDL's exported window on 5+, picked at startup.
- When SAM launches a native app, stdout and stderr go to `/dev/null`. Nothing you
  print survives until you `freopen` a log file.
- The Back button doesn't arrive until the window asks for it, through the
  `SDL_WEBOS_ACCESS_POLICY_KEYS_BACK` hint, and only if it's set **before** the
  window is created. It then arrives as scancode 482.
- Don't link libcurl. The SDK has `.so.4`, the TV has `.so.5`, and the binary won't
  start. `dlopen` it at runtime and try both.
- The TV's libcurl (7.53) is old: reusing connections helps a lot, but only after
  a transfer finished cleanly.
- Firmware symbol dumps from [webosbrew](https://github.com/webosbrew/dev-toolbox-cli)
  tell you whether a binary will **load** on a TV you don't own. They don't tell
  you it works. That's how webOS 3 support was checked: exactly one missing
  symbol (`SDL_CreateRGBSurfaceWithFormat`, SDL 2.0.5), replaced in
  `src/sdlcompat.h`, so the same package runs on webOS 3 and up.

And on Samsung, where the app is WebAssembly in the TV's browser: one busy main
thread and the whole app waits. Most of the Samsung speedups came from moving
work off it (image decoding in a Worker, no synchronous storage writes, no
per-frame file checks).

---

## Documentation

Most design notes are in Portuguese, because I wrote them for myself:
[PORT-LEGACY.md](PORT-LEGACY.md), [PLANO-CONTA-SYNC.md](PLANO-CONTA-SYNC.md),
[MEDIDAS-WEB.md](MEDIDAS-WEB.md), [FERRAMENTAS.md](FERRAMENTAS.md).
Release notes live in `docs/releases/`. [INSTALL.md](INSTALL.md) and this file are
in English.

## Disclaimer

Nuvio Legacy is a **media player**. It does not host, store, index, upload or
distribute any movies, series, channels or other content, and it ships with no
content sources. Everything it plays comes from add-ons, playlists, servers and
accounts that **you** choose to install or connect. Those are run by third
parties that have nothing to do with this project.

- You are responsible for what you add and watch, and for making sure you have
  the right to access it where you live.
- This project does not control, endorse or verify any add-on, playlist or
  service, and does not provide links to unlicensed content.
- There is nothing hosted here to take down. Copyright notices about a stream
  should go to whoever hosts it (the add-on, playlist or server), not to this
  repository.
- Nuvio Legacy is not affiliated with Stremio, the official Nuvio app, any
  streaming service, debrid service or TV maker.

## License

[GNU General Public License v3.0](LICENSE).

The C in `src/` is written from scratch, but the interface icons in
`deploy/app/art/icones/` come from the SVGs of the upstream web app, which is
GPLv3, as is the [web fork](https://github.com/iqui27/NuvioTVSmart-legacy-webos)
this grew alongside. So the whole thing goes out under the same terms.

Settings icons: [Lucide](https://lucide.dev), ISC license. The unmodified SVGs
and the license text are in `deploy/app/art/icones/lucide/`; the `aj_*.png` next
to them are rasterized by `tools/icones-lucide.sh`.

DTS-enabled builds use libraries from the [FFmpeg project](https://ffmpeg.org/)
under **LGPL 2.1 or later**. `tools/build-dts-ffmpeg.sh` records the exact release
source URL and checksum; `tools/arm.sh` includes its license and provenance in
`licenses/dts/` inside the app package. The
[DTS distribution notes](docs/features/dts/README.md#dependency-provenance-and-distribution)
describe the corresponding source and static relinking materials needed with
binary releases.

"Nuvio", the logo and the wordmark belong to the original authors. The GPL covers
the code, **not** the name or the branding. They appear here only to say which
project this one comes from.

Artwork fetched at runtime (TMDB, Trakt, Apple TV, addon CDNs) belongs to its
owners and isn't covered by this license.
