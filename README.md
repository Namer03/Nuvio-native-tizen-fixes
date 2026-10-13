# Nuvio Native Legacy (Tizen Fixes Fork)

This is a custom fork of [iqui27/nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy), an unofficial native TV client for LG webOS, Samsung Tizen, and Android TV built using C, SDL2, and OpenGL ES 2.0.

This fork focuses on stability fixes and new features for **Samsung Tizen devices**, with a particular focus on Tizen 6 and Tizen 9. Testing has been performed on Samsung S95C hardware.

> ⚠️ **Development Status: Active Testing and Setup**
>
> Application changes are being integrated and validated. Live feature validation and runtime testing are pending local Nuvio Supabase backend key configuration.

---

## 🛠️ Main Application Fixes

- **Regex Engine Stability:** Core regular-expression stability fixes have been integrated into the main application logic. **Status:** Integrated; awaiting validation.

## ✨ Features and Roadmap

- **QR Code Local Log Exporter:** Generates an on-screen QR code to simplify exporting application logs from physical TV hardware for debugging. **Status:** Built; awaiting validation.
- **Microphone Voice Search:** Adds microphone input capabilities for voice-activated content searches directly within the Tizen interface. **Status:** Current focus.
- **DTS and Dolby TrueHD Downmixing:** Implements audio downmixing to convert high-definition audio tracks into compatible stereo formats, helping prevent silent playback and audio-related playback failures on native TV speakers. **Status:** Planned.

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
