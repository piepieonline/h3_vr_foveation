# VRFoveationFix

Vibed ZHMModSDK port of [RealChrizzl/hitman-vr-foveation-fix](https://github.com/RealChrizzl/hitman-vr-foveation-fix)

Edge-to-edge sharpness for HITMAN World of Assassination in PC VR.

HITMAN renders VR with fixed foveation — four layers per frame, two wide ones at half resolution
covering the whole field of view and two narrow ones at full resolution covering only a small circle
in the centre. Everything outside that circle is upscaled from the half-resolution layer, which is
what makes the periphery look like mush on a pancake headset.

This mod switches the game to two layers at full resolution covering the whole field of view. That
is twice the pixel work, but the pixel _density_ is within about a percent of the old sweet spot —
so you get the old centre-of-image sharpness everywhere.

Two layers alone would break things, so the mod also does the two repairs upstream found for it. The
render context keeps reporting four views, because geometry and visibility need it — a car wheel, a
door, a whole façade otherwise pops in and out as you move. The one place that must _not_ see four
is the refraction depth copy, which multiplies its draw range by that count; left at four it renders
the narrow foveal views into glass, flowing water, bottles, NPC glasses and some emissive lights,
differently in each eye. The mod detours that copy and the pass around it, so it alone runs on the
two physical eyes.

Both VR backends are covered: Oculus (LibOVR) and SteamVR (OpenVR). The game has no OpenXR backend.

## Usage

Enable the mod in the Mod Selector, then start VR and load a mission. Progress is reported in the
SDK log. The only UI is a **VR FOVEATION** button in the SDK menu bar, which dumps a diagnostic
report to the console: the game build, the loaded VR runtime, where every signature landed and the
current contents of the VR device. It reads only, and is the same report as the upstream project's
`HitmanVRProbe`, so paste it into an issue if the fix does not work on your build or headset.

The game rebuilds its render state during mission, scene and save-game loads, and can put its own
foveation values back faster than a frame update can catch — measurably so under Proton. Once the
first write has been confirmed, a guard thread re-checks the 24 bytes that matter about once a
millisecond and writes nothing while they are right. Everything it does go on to write goes through
the same validation, verification and rollback as the frame update.

The mod has to be loaded **before VR initialises** — not necessarily before the game starts. Turning
it on mid-session is fine as long as you have not started VR yet in that session. If VR is already
running when the mod loads it refuses, logs why, and changes nothing; restart the game in that case.

Set `enabled = false` under `[general]` in `VRFoveationFix.ini` (next to the mod DLL) to turn it off
without unloading it.

## Credit and maintenance

Port of [RealChrizzl/hitman-vr-foveation-fix](https://github.com/RealChrizzl/hitman-vr-foveation-fix)
(MIT), which is where the reverse engineering was done — up to and including its v1.4, whose
refraction split and renderer guard this mod carries. That project's `docs/HOW-IT-WORKS.md` explains
what each site does, and `docs/UPDATING.md` describes how to find them again after a game update —
the signatures live in [src/Layout.h](src/Layout.h) and nowhere else.

Everything is verified against build 3.270.1. Every signature has to match exactly once or the mod
changes nothing at all and says so in the log, and that includes the three refraction sites: a build
where only the foveation sites still match is a build where glass and water would be wrong in
stereo, which is not an improvement worth applying.

Upstream is an external tool that writes into the game from another process. In here, the same work
is a mod: no administrator rights, no polling for the process, no race with VR start-up, and the
refraction split is two detours rather than hand-assembled wrappers spliced into call sites.

### Currently tracked version

https://github.com/RealChrizzl/hitman-vr-foveation-fix/commit/672eae44817f675c5e56ac809475a5e9cb1a9196

[PLAN.md](PLAN.md) documents the design of this port, including the not-yet-taken path of moving the
signatures and device layout into SDK core as `Globals`/`Hooks`/Glacier types.

## Building on Windows

Open the folder in Visual Studio and pick the `x64-Debug` or `x64-Release` preset, or from a
developer prompt:

```cmd
cmake --preset x64-Release .
cmake --build _build/x64-Release --parallel
```

To install into the game folder, add a `CMakeUserPresets.json` that inherits the preset and sets
`GAME_INSTALL_PATH`, then run the install step as well.

## Building on Linux (cross-compiling to Windows)

There is no Linux build of the game, so this cross-compiles a Windows DLL with `clang-cl` +
`lld-link` against the real MSVC CRT and Windows SDK headers, which
[xwin](https://github.com/Jake-Shadle/xwin) downloads from Microsoft.

The Nix dev shell and the xwin fetch script live in the ZHMModSDK repository, so a local checkout of
the SDK is required — the released `DevPkg-ZHMModSDK.zip` does not contain them. The `clang-cl`
toolchain file and the vcpkg triplet are vendored here in [cmake/](cmake/).

You will need [Nix](https://nixos.org/download/) with flakes enabled. Everything else (CMake, Ninja,
clang, lld, vcpkg, xwin) comes from the dev shell.

### 1. Clone this repository and the SDK.

```sh
git clone --recurse-submodules https://github.com/piepieonline/VRFoveationFix.git
git clone --recurse-submodules https://github.com/OrfeasZ/ZHMModSDK.git
```

### 2. Fetch the MSVC CRT and Windows SDK.

From the ZHMModSDK directory:

```sh
nix develop --command cmake/scripts/fetch-xwin.sh
```

This only needs to be done once. It leaves roughly 1.7 GB in `ZHMModSDK/.xwin`. The dev shell
exports `XWIN_SPLAT_DIR` pointing at it, which is how [cmake/toolchains/clang-cl.cmake](cmake/toolchains/clang-cl.cmake)
finds the headers and import libraries.

### 3. Create `CMakeUserPresets.json` in this repository.

This file is gitignored because the paths are machine-specific. Substitute your own paths for
`/path/to/ZHMModSDK` and the game directory:

```json
{
    "version": 3,
    "configurePresets": [
        {
            "name": "x64-Release-Cross",
            "displayName": "Cross-compilation from Linux to Windows",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/_build/${presetName}",
            "architecture": { "value": "x64", "strategy": "external" },
            "condition": {
                "type": "equals",
                "lhs": "${hostSystemName}",
                "rhs": "Linux"
            },
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "RelWithDebInfo",
                "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded",
                "CMAKE_INSTALL_PREFIX": "${sourceDir}/_install/${presetName}",
                "CMAKE_TOOLCHAIN_FILE": {
                    "value": "${sourceDir}/vcpkg/scripts/buildsystems/vcpkg.cmake",
                    "type": "FILEPATH"
                },
                "VCPKG_OVERLAY_TRIPLETS": "${sourceDir}/cmake/vcpkg-overlays",
                "VCPKG_TARGET_TRIPLET": "x64-windows-zhm-cross",
                "VCPKG_HOST_TRIPLET": "x64-linux",
                "VCPKG_CHAINLOAD_TOOLCHAIN_FILE": "${sourceDir}/cmake/toolchains/clang-cl.cmake",
                "VCPKG_INSTALL_OPTIONS": "--allow-unsupported",
                "VCPKG_APPLOCAL_DEPS": "OFF",
                "CMAKE_POLICY_DEFAULT_CMP0091": "NEW",
                "ZHMMODSDK_DIR": "/path/to/ZHMModSDK",
                "ZHMMODSDK_PRESET": "x64-Release-Cross"
            }
        },
        {
            "name": "x64-Release-Cross-Install",
            "inherits": ["x64-Release-Cross"],
            "cacheVariables": {
                "GAME_INSTALL_PATH": "/path/to/SteamLibrary/steamapps/common/HITMAN 3"
            }
        }
    ]
}
```

Three of those settings are worth explaining:

- `VCPKG_TARGET_TRIPLET` names the overlay triplet in
  [cmake/vcpkg-overlays/](cmake/vcpkg-overlays/), which chainloads the `clang-cl` toolchain for
  every port so the mod's only dependency (`directx-headers`) is built for Windows.
- `VCPKG_HOST_TRIPLET` has to be `x64-linux` — vcpkg's own host tools are native.
- `ZHMMODSDK_PRESET` must name a cross preset. Without it the SDK builds itself with the default
  `x64-Release` preset, which uses `cl.exe` and cannot configure on Linux.

### 4. Build.

The dev shell derives `XWIN_SPLAT_DIR` and `VCPKG_ROOT` from the working directory, so it must be
entered from the ZHMModSDK directory, then changed into this one:

```sh
cd /path/to/ZHMModSDK
nix develop --command bash -c '
    cd /path/to/VRFoveationFix &&
    cmake --preset x64-Release-Cross . &&
    cmake --build _build/x64-Release-Cross --parallel
'
```

Configuring builds and installs the SDK first ([cmake/setup-zhmmodsdk.cmake](cmake/setup-zhmmodsdk.cmake)
shells out to a nested SDK build), so the first run takes a while. The result is
`_build/x64-Release-Cross/VRFoveationFix.dll`.

The DLL is much larger than an MSVC-built one because the toolchain emits DWARF alongside CodeView,
so `gdb` on the Linux side can read symbols directly out of the DLL while the game runs under
Proton.

### 5. Install (optional).

Use the `-Install` preset and run the install step:

```sh
cd /path/to/ZHMModSDK
nix develop --command bash -c '
    cd /path/to/VRFoveationFix &&
    cmake --preset x64-Release-Cross-Install . &&
    cmake --build _build/x64-Release-Cross-Install --parallel &&
    cmake --install _build/x64-Release-Cross-Install
'
```

[.vscode/tasks.json](.vscode/tasks.json) wraps these same commands, so `Ctrl+Shift+B` runs build +
install from the editor. `cross: configure` is the one to run after editing `CMakeLists.txt` or the
presets.

Note that this copies `VRFoveationFix.dll` into `Retail/mods` **and** overwrites
`Retail/ZHMModSDK.dll` with the locally built SDK. That is deliberate — the mod is compiled against
your local SDK's headers and import library, so the matching runtime has to be installed with it.
Any other mods in that folder were built against whatever SDK you had before, so you may want to
rebuild those too.
