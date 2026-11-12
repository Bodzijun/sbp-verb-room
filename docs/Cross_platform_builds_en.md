# macOS and Linux VST3 builds

The **VST3 builds** GitHub Actions workflow builds and tests two experimental targets:

| Package | Runner | CPU target | AVX2/FMA required |
|---|---|---|---|
| macos-universal | macos-15 | arm64 + x86_64; macOS deployment target 11.0 | No |
| linux-x86_64 | ubuntu-24.04 | x86_64 | No |

These targets need actual DAW testing on their respective operating systems. Successful compilation and automated processor tests do not establish compatibility with every host or distribution.

## Download builds

1. Open the repository's **Actions → VST3 builds** page.
2. Select a successful run for the desired source commit.
3. Download `VerbRoom-macos-universal` or `VerbRoom-linux-x86_64` from **Artifacts**. GitHub may require you to sign in.
4. Extract the artifact, then extract the enclosed platform archive. Keep the whole `Verb Room.vst3` bundle/directory intact.

Each archive contains the plug-in, presets, documentation, license, `INSTALL.txt`, and `BUILDINFO.json` identifying the source commit. A SHA-256 checksum accompanies the archive. The macOS ZIP and Linux tar.gz preserve bundle permissions; do not distribute a bare executable copied out of the bundle.

The workflow runs for code/build changes pushed to main, version tags, relevant pull requests, and **Run workflow** requests. It uploads build artifacts; it does not publish a GitHub Release automatically. Artifacts expire after 30 days, so download release candidates before they expire.

## Installation

Close the DAW before replacing any installed plug-in.

- **macOS:** copy `Verb Room.vst3` to `~/Library/Audio/Plug-Ins/VST3/` for the current user, or `/Library/Audio/Plug-Ins/VST3/` for all users.
- **Linux:** copy `Verb Room.vst3` to `~/.vst3/`. The Ubuntu 24.04 build requires compatible system libraries, including glibc 2.39 or newer; older distributions are not targeted.

Reopen the host and rescan plug-ins if needed. Existing project states and `.verbroom` format 1 presets are unchanged.

### macOS signing

The CI verifies both architecture slices and applies an **ad-hoc signature**. This is not a Developer ID signature or Apple notarization. macOS may block downloaded plug-ins; these packages are intended for compatibility testing.

Public distribution with Developer ID signing and notarization requires the developer's Apple credentials/certificates and a separate signing setup. No Apple credentials are required for these test builds, and none are included in this workflow.

## Build locally

Clone with the JUCE submodule:

```sh
git clone --recurse-submodules https://github.com/Bodzijun/sbp-verb-room.git
cd sbp-verb-room
```

On Linux, install the dependencies listed in `.github/workflows/vst3.yml`. On macOS, install Xcode command-line tools, CMake, and Ninja.

```sh
# Linux x86_64, portable CPU target
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVERB_BUILD_PLUGIN=ON -DVERB_AVX2=OFF

# macOS Universal (use this configure command instead of the Linux one)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVERB_BUILD_PLUGIN=ON -DVERB_AVX2=OFF \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

cmake --build build --target VerbPlugin_VST3 VerbPluginTest tail_regression quality_test energy_test stage2_test tail_transition_test --parallel 2
# Linux headless tests (the 15-minute stage2_fixes regression is not built here):
xvfb-run -a ctest --test-dir build --output-on-failure -E '^stage2_fixes$'
# macOS tests:
ctest --test-dir build --output-on-failure -E '^stage2_fixes$'
```

Use separate build directories when changing platform or architecture. `VERB_AVX2` defaults on for x86-only builds and off for ARM/universal builds. A build configured with it on still requires AVX2/FMA at runtime. The runtime CPU check is conditional on that build option; portable builds do not reject Apple Silicon. No AVX2 flag is passed to an ARM slice. The core static library uses position-independent code so it can link into Linux VST3 shared objects.

The existing Windows configuration continues to default to AVX2/FMA. Setting up these workflows does not replace the user's installed Windows plug-in.
