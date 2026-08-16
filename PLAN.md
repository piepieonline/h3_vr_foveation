# Upstreaming VRFoveationFix into ZHMModSDK

> The mod is built, shipped, and caught up with upstream v1.4. It is self-contained: every address,
> signature and offset lives in `src/`, and the only SDK API it uses is the public one.
>
> This document is now **only the SDK-side plan** — what has to land in ZHMModSDK core so the mod
> reads like the rest of `Mods/`, and what each piece deletes from the mod in exchange. The design
> history (why the self-contained version was built first, the full mod design, the verification
> matrix) is in git: `fe823f3` and `a54b511`, plus the comments in `src/Layout.h`.

## Where the knowledge lives today

| File | Lines | Portable to the SDK? |
|---|---|---|
| `src/Layout.h` | 224 | Mostly — device offsets, the device locator, the refraction call sites |
| `src/CodePatcher.h/.cpp` | 303 | Yes — duplicates `Util::ProcessUtils::SearchPattern`, which mods cannot reach |
| `src/RefractionFix.h/.cpp` | 326 | Partly — the scanning and MinHook plumbing go; the swap logic stays as a detour body |
| `src/MsvcIntrinsics.cpp` | 37 | Yes — exists only because the mod links MinHook |
| `src/VRFoveationFix.h/.cpp` | 848 | No — policy, state machine, guard, diagnostics |
| `src/ProcessInfo.h/.cpp` | 108 | No — build identification for the diagnostics report |

Verified against `HITMAN3.exe` build 3.270.1 (PE timestamp `1781013974`); all nine signatures match
exactly once each.

---

## 1. `ZRenderVRDevice` — Glacier type

**Lands in** `ZHMModSDK/Include/Glacier/ZRenderVRDevice.h` (new); `ZRender.h` already exists and is
where the forward declaration goes. Style follows `Include/Glacier/EntityFactory.h`: `PAD(...)`
fillers with the running offset in a trailing comment.

Offsets are in `src/Layout.h:152-191` and are verified on both backends — Oculus and OpenVR share
the layout. The **field names are not**; they are inferred from behaviour and have to be sanity
checked before a PR, because a `PAD(0x319)`-shaped struct with guessed names in a public header is a
long-lived maintenance commitment.

```cpp
class ZRenderVRDevice {
public:
    PAD(0x319);                          // 0x000
    bool m_bActive;                      // 0x319
    bool m_bWideNarrowOverlay;           // 0x31B
    PAD(0x104);                          // 0x31C
    float m_aFovTangents[4];             // 0x420
    PAD(0x60);                           // 0x430  geometry block
    float m_aOverlayScaleRatios[4];      // 0x490
    PAD(0x20);                           // 0x4A0
    float m_fOverlayPassBlend;           // 0x4C0
    float m_fOverlayCircleRadius;        // 0x4C4
    PAD(0x10);                           // 0x4C8
    uint32_t m_nTransitionState;         // 0x4D8
    PAD(0x34);
    uint32_t m_nWidth;                   // 0x510
    uint32_t m_nHeight;                  // 0x514
    PAD(0x8);
    uint16_t m_nLayerCount;              // 0x520
    PAD(0xE);
    void* m_pRenderTexture;              // 0x530
    void* m_pRenderView;                 // 0x538
};
```

**Deletes from the mod:** `Layout.h:152-191` (the twelve `Device*` offset constants plus
`DeviceMinSize`/`DeviceDiagnosticSize`), and turns every raw offset read in `VRFoveationFix.cpp` into
a named field access. The plausibility bounds (`MinFovTangent`, `MinScaleRatio`, …) **stay** — they
are the mod's validation policy, not layout.

## 2. `Globals::RenderVRDevice`

**Lands in** `Include/Globals.h` (forward declaration, next to `ZRenderManager`) and `Src/Globals.cpp`
via `PATTERN_RELATIVE_GLOBAL` (`ZHMModSDK/Src/GlobalsImpl.h:40`), using the locator from
`Layout.h:139-150` at relative offset 3:

```
48 8B 0D ?? ?? ?? ?? 8B D6 48 8B 01 44 38 B9 1B 03 00 00 0F 84
```

The `1B 03 00 00` displacement must stay fixed in the mask — it encodes the `0x31B` flag offset and
is most of what makes the pattern unique.

One thing to settle during implementation: whether the slot holds the device directly or the VR
manager. Upstream's verified path is manager `0x03225D20` → device at `+0x141A0`, while this locator
dereferences straight to something that passes the device plausibility check. If it is the manager,
declare `ZRenderVRManager` with `ZRenderVRDevice* m_pDevice; // 0x141A0` and expose that instead.

**Deletes from the mod:** `Layout::DeviceLocatorPattern` and its two decode offsets, the `rel32`
resolution and `m_DeviceSlot`/`m_WnoOffset` members, and `MaxPlausibleFieldOffset`. `GetDevice()`
keeps its FOV-tangent plausibility check — the SDK global can be null or half-built, and the mod is
the thing that decides when it is safe to write.

## 3. Hooks for the WNO writer and the FOV limit

Four of the five byte patches become detours, which is strictly better: no `VirtualProtect`, teardown
handled by the SDK on unload, and other mods can compose with them.

- **WNO writers A and B** — one `PATTERN_HOOK` on the enclosing function, anchored on the
  `8B 97 D8 04 00 00 83 FA 01` prologue shared by both signatures in `Layout.h:42-52`. The mod calls
  the original, then clears `m_bWideNarrowOverlay`. Replaces two patches with one hook.
- **FOV limit, ×2** — the same method compiled into both device classes at vtable slot `+0x208`, so
  either two `PATTERN_HOOK`s using the existing 44-byte signatures (`Layout.h:53-64`) or one
  `PATTERN_VTABLE_HOOK` per class. The mod returns `HookAction::Return(1)`.

**Does not move:** the view-count site (`Layout.h:65-69`). It is a `cmpb`/`cmovne` pair mid-function;
detouring the enclosing function means reimplementing its view-count push against a private render
context. It stays a 7-byte patch, and needs no SDK change — `PatchCodeStoreOriginal`
(`Include/IModSDK.h:251`) is already exported and already reverts on unload.

**Deletes from the mod:** four of five entries in `Layout::CodeSites` and their fix-byte arrays.

## 4. Hooks for the refraction split — this is the one that removes MinHook

The refraction depth copy has to see a view count of 2 while everything around it sees 4; see
`src/Layout.h:72-80` and `src/RefractionFix.h` for why. It needs detours at two addresses that are
reached by decoding the `E8` of a located call, and the SDK's `Hooks` is a fixed exported set — so
today the mod links its own copy of MinHook, which is what drags in `src/MsvcIntrinsics.cpp` (MinHook's
trampoline builder calls `__movsb`, which clang-cl leaves undefined when cross-compiling; the SDK
carries the same shim for the same reason).

The SDK already has the right mechanism: **`PATTERN_RELATIVE_CALL_HOOK`** (`Src/HookImpl.h:634`)
locates a pattern, requires the match to *begin* on an `0xE8`, decodes the rel32 and hooks the target
— exactly what `RefractionFix::Install` does by hand.

Two entries are needed:

- **`ZRenderContext_CopyRefractionDepth`** — 6 integer/pointer arguments. Both call sites in
  `Layout::RefractionCopyCalls` resolve to the same function, so one hook covers both.
- **`ZRenderContext_DrawRefractiveAndTransparent`** — 10 integer/pointer arguments, no floats (its
  one call site writes `[rsp+0x20]` … `[rsp+0x48]` and no xmm). Needed only to learn the render
  context, which is its first argument. Argument counts must be exact, because a detour passes them
  through.

The patterns cannot be transplanted verbatim: the mod's are anchored ahead of the call
(`CallOffset` 13 and 34), and `PatternRelativeCallHook` needs the match to start at the `E8`. Both
copy sites already carry disambiguating bytes *after* the call, so re-anchoring as
`E8 ?? ?? ?? ?? 48 8B 9D D0 01 00 00 …` should hold — this must be re-verified for uniqueness on the
target build, and the scope pass may need `PATTERN_HOOK` on the function prologue instead if the
trailing context is too short.

**Deletes from the mod:** all of `RefractionFix`'s scanning, target-agreement checking and MinHook
plumbing, plus `src/MsvcIntrinsics.cpp` entirely and the `minhook` dependency in `vcpkg.json`. What
remains is roughly 40 lines of detour body — the `thread_local` scope, the count swap at
`ContextCountTop`, and the three counters the diagnostics report prints — declared with
`DECLARE_PLUGIN_DETOUR` / `DEFINE_PLUGIN_DETOUR` (`Include/IPluginInterface.h:218`).

## 5. A unique-match scan or patch API

`Util::ProcessUtils::SearchPattern` lives under `ZHMModSDK/Src/`, is not installed and not
`ZHMSDK_API`-exported (`Include/Util/` has only `HttpUtils`, `ImGuiUtils`, `ResourceUtils`,
`StringUtils`), and `SDK()->PatchCode` (`Include/IModSDK.h:90`) stops at the first match. Refusing to
patch an ambiguous signature is the safety property this mod cares about most, so it carries its own
scanner.

Smallest useful addition: a `PatchCodeStoreOriginal` variant that fails on more than one hit, or an
exported scan returning a saturating hit count. Any mod doing byte patching wants this, so it is the
piece most likely to be accepted on its own merits, independent of anything VR.

**Deletes from the mod:** `ScanCode`, `ParsePattern` and `MemoryPatch` — most of
`src/CodePatcher.h/.cpp`. `IsReadable` **stays**: it guards every device read in the frame update,
the guard thread and the diagnostics dump (`VRFoveationFix.cpp:240`, `:246`, `:269`, `:343`, `:396`,
`:617`, `:629`, `:659`).

---

## 6. What stays in the mod, whatever lands

Not portable, and shouldn't be: this is policy, not knowledge about the game.

- The state machine and transition-only logging (`Inert` → `WaitingForVR` → `WaitingForMission` →
  `Active` / `Failed`), and the `enabled` setting.
- `SyncDeviceFields` — validate before capturing, write before `+0x319` says active, read back and
  roll back, forget on device change. One validated write path under one lock.
- The renderer guard thread. The game restores its own foveation values during mission, scene and
  save-game loads inside a window upstream measures at up to 15 ms; a frame update samples at ~11 ms
  at 90 fps and worse during a load. The guard re-checks 24 bytes about once a millisecond, writes
  nothing while they are right, and resolves the device pointer from the game's global every pass.
- `LogDiagnostics` and `ProcessInfo` — the pasteable report matching upstream's HitmanVRProbe.
- The view-count byte patch (§3).

## 7. The blocker

`PATTERN_HOOK` and `PATTERN_RELATIVE_GLOBAL` resolve at static init whether or not any mod uses them,
and each failure calls `Fail()` (`Src/HookImpl.h`, `Src/GlobalsImpl.h:22`, counter in
`Src/Failures.h:4`). At `g_Failures >= 3` the SDK logs "Too many errors occurred" and unloads itself
(`Src/ModSDK.cpp:731-732`).

Pieces 2, 3 and 4 add **four to six** patterns to that path. A game update that moves only the VR
renderer would then push a flatscreen-only user over the threshold and unload the SDK for them. This
is the first thing that will be raised in review, and it is a fair objection.

So §2-4 realistically depend on one of: lazy/on-demand resolution for patterns a mod actually asks
for, an opt-in group that does not count toward `g_Failures`, or maintainer acceptance that VR
patterns are exempt. **Piece 5 has no such dependency** and can go first on its own.

The secondary cost is maintenance: SDK patterns are refreshed every game release, and VR ones can
only be validated by someone with a headset and both runtimes.

## 8. Sequencing

1. **Piece 5** (unique-match scan/patch). No VR content, no failure-path risk, immediately deletes
   ~300 lines from this mod and helps every other patching mod.
2. **Piece 1** (Glacier type). Header-only, adds nothing to the failure path. Needs the field names
   sanity checked first.
3. Resolve §7 with the maintainers.
4. **Piece 2**, then **3**, then **4**, deleting mod-local code as each lands.

Each step is independently shippable and leaves the mod working. Nothing here should be attempted
until the current signatures have survived a game update or two — the argument for upstreaming is
much easier to make about patterns with a track record.

## 9. Build-side consequences

- `vcpkg.json` loses `minhook` once §4 lands. `directx-headers` stays regardless: the SDK's own public
  headers include `<directx/d3d12.h>` (`Include/Hooks.h:17`, `Include/Glacier/ZRender.h:3`).
- `cmake/vcpkg-ports/directx-headers` — the overlay port that skips the pkg-config check — stays for
  as long as `directx-headers` does. It exists because the upstream port's `vcpkg_fixup_pkgconfig()`
  makes a Windows host fetch pkgconf from the msys2 mirrors, and msys2 only keeps current packages,
  so a pinned vcpkg's `msys2-runtime` starts 404ing in CI once it is superseded.
- The root `CMakeLists.txt` line adding `VRFoveationFix` to `set(MODS ...)` is the only in-tree
  change if the mod ever moves into the SDK repo; the target name must equal the folder name.
