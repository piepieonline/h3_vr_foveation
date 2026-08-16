# Port hitman-vr-foveation-fix into ZHMModSDK as `VRFoveationFix`

> Design document for this mod. Kept in-tree so the reasoning behind the offsets, and the
> not-yet-taken upstream path (Option B), stay next to the code.
> **Status: Option A built, and caught up with upstream v1.4 — see §9.**

## Context

[RealChrizzl/hitman-vr-foveation-fix](https://github.com/RealChrizzl/hitman-vr-foveation-fix) is a
PowerShell tool that removes HITMAN's fixed-foveation (Wide/Narrow Overlay) blur in PC VR. It runs
as an admin process, `OpenProcess`/`WriteProcessMemory`s into a running `HITMAN3.exe`, applies five
instruction patches before VR initialises, then keeps three float/flag fields in the VR device
struct pinned once a mission loads. Everything is in-memory and reverted on exit.

In-process under the SDK this gets much simpler: no admin, no polling loop, no race with VR startup
(mods load before engine init), and `Globals::GameLoopManager` provides the per-frame tick for the
device-field maintenance.

Decisions taken: mod lives in-tree at `Mods/VRFoveationFix`; **pattern-scan only** (drop the
hardcoded build-3.270.1 RVA fast path); **no UI** — fully automatic, status via `Logger` only.

The open question was where the address knowledge lives. **Option A** keeps it all in the mod.
**Option B** upstreams it into SDK core as Glacier types + `Globals` + `Hooks`, so the mod contains
no addresses — the way every other mod in `Mods/` is written.

**Decided: build Option A first.** Option B is documented below as the follow-up; see §5 for the
sequencing and §4 for why B cannot go first.

---

## 1. What has to happen, independent of option

Five code sites, applied once before VR comes up (from upstream `docs/HOW-IT-WORKS.md`):

| Site | Fix bytes | Effect |
|---|---|---|
| WNO flag writer A | `B1 00 90` | two layers instead of four |
| WNO flag writer B | `B0 00 90` | ditto |
| FOV limit, Oculus device | `B8 01 00 00 00 90 90` | full FOV, no black border |
| FOV limit, OpenVR device | `B8 01 00 00 00 90 90` | same, other backend |
| view count | `48 85 E4 90 90 90 90` | force count 4; without it geometry pops |

Writers A/B live in one function that sets `device+0x31B` from `[rdi+0x4D8] == 1`. The two FOV-limit
sites are the same method compiled into two device classes, both at vtable slot `+0x208`. The view
count site is a `cmpb $0, 0x31B(%rax)` feeding a `cmovne` in the middle of a larger function.

Three device fields, maintained per frame once VR is active, restored from a snapshot on unload:
`+0x490` 4×`1.0f` (scale ratios), `+0x4C0` and `+0x4C4` = `0` (overlay pass / centre circle off).

Read-only fields for state: `+0x319` active, `+0x31B` WNO flag, `+0x420` 4× FOV tangents
(plausibility check), `+0x4D8` transition, `+0x520` layer count, `+0x530` texture pointer.

---

## 2. Option A — everything in the mod (accepted)

Only change outside the mod: one line in the root `CMakeLists.txt`.

**`Mods/VRFoveationFix/CMakeLists.txt`** — verbatim copy of `Mods/NoPause/CMakeLists.txt` with the
target renamed. Links only `ZHMModSDK`.

**`Mods/VRFoveationFix/Src/CodePatcher.h/.cpp`** (~120 lines)
`Util::ProcessUtils::SearchPattern` sits under `ZHMModSDK/Src/`, is not installed and not
`ZHMSDK_API`-exported, so a mod cannot reach it. `SDK()->PatchCode()` *is* exported but stops at the
first match — and refusing on an ambiguous signature is the safety property most worth keeping from
upstream. So the mod carries its own scanner:

- `Scan(pattern, mask)` over `[BaseOfCode, BaseOfCode + SizeOfCode)` of `GetModuleHandleW(nullptr)`,
  read from the loaded PE optional header — the same range `ModSDK::PatchCode` uses
  (`ZHMModSDK/Src/ModSDK.cpp:206-251` is a copy-pasteable reference). Stops at 2 hits; the caller
  treats `!= 1` as a hard failure.
- `Patch { uintptr_t addr; std::vector<uint8_t> original, fix; }` with `Apply()`/`Revert()` doing
  `VirtualProtect` → `memcpy` → restore protection → `FlushInstructionCache`.
- Patterns kept as the upstream `"8B 97 D8 04 ?? .."` strings, parsed at runtime, so they can be
  pasted straight from the upstream repo when a game update moves things.

**`Mods/VRFoveationFix/Src/VRFoveationFix.h/.cpp`**
Structured like `Mods/DebugCheckKeyEntityEnabler` (patch in `Init`, revert in the destructor) plus a
frame update like `Mods/Clumsy/Src/Clumsy.cpp:20-39`.

- `Init()`
  1. `GetSettingBool("general", "enabled", true)`, seeding it if absent (pattern from
     `Mods/Randomizer/Src/Randomizer.cpp:59-69`). Bail out if disabled.
  2. Scan the five patch signatures plus the device locator
     (`48 8B 0D ?? ?? ?? ?? 8B D6 48 8B 01 44 38 B9 1B 03 00 00 0F 84`). Any signature not hitting
     exactly once → `Logger::Error` naming the site, apply nothing, stay inert.
  3. From the locator: `rel32` at `+3` resolves the global slot holding the device pointer; the
     `u32` displacement at `+15` is the WNO flag offset. Reject if outside `(0, 0x4000]`.
  4. Refuse if VR is already up (device non-null, plausible, `+0x319 == 1`) — covers live-loading
     the mod mid-session from the Mod Selector.
  5. Apply the five patches capturing originals; read back and verify, reverting all five if any did
     not stick.
- `OnEngineInitialized()` — `RegisterFrameUpdate(delegate, 1, EUpdateMode::eUpdateAlways)`;
  `Globals::GameLoopManager` is not reliably available in `Init()`.
- `OnFrameUpdate(const SGameUpdateEvent&)` — deref the device slot; plausibility-check the four FOV
  tangents at `+0x420` against `[0.2, 3.0]`; require `+0x319 == 1` and the WNO byte `== 0`; reset
  cached state when the texture pointer at `+0x530` changes; then, if `+0x490`/`+0x4C0` are not
  already at the fixed values, snapshot the stock bytes once and write the fixed ones.
- State machine → log. `Inert`, `WaitingForVR`, `WaitingForMission`, `Active`, `Failed`;
  `Logger::Info` **only on transition**, never per frame. Upstream's `NeedsMissionReload` — the case
  where `+0x4D8` was already `3` when the fields were first written — is deliberately **not** here;
  the guard in §9 closes the window it was reporting on.
- Destructor — unregister the frame update, restore device fields from the snapshot, revert the five
  code patches. This is the SDK's uninstall hook (`ZHMModSDK/Src/ModLoader.cpp:333-364`).

**`CMakeLists.txt` (root, ~line 43)** — add `VRFoveationFix` to `set(MODS ...)`; target name must
equal the folder name.

---

## 3. Option B — upstream the addresses into SDK core (not taken yet)

Same mod behaviour, but no addresses, offsets, or scanner in the mod. The knowledge moves into the
three places SDK core already keeps it, and the mod becomes ~80 lines that read like `SkipIntro`.

### 3a. `ZHMModSDK/Include/Glacier/ZRenderVRDevice.h` (new)

Declare the device layout with named fields and `PAD(...)` fillers, following
`Include/Glacier/EntityFactory.h` (`PAD(0x10); // 0x10` — pad, then the running offset in a comment):

```cpp
class ZRenderVRDevice {
public:
    PAD(0x319);                          // 0x000
    bool m_bActive;                      // 0x319
    bool m_bWideNarrowOverlay;           // 0x31B  (offset confirmed by the locator pattern)
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
};
```

Field names are guesses from behaviour and must be sanity-checked before a PR; the offsets
themselves are verified. `ZRender.h` already exists and is where a forward declaration would go.

### 3b. `Globals.h` / `Src/Globals.cpp`

```cpp
PATTERN_RELATIVE_GLOBAL(
    "\x48\x8B\x0D\x00\x00\x00\x00\x8B\xD6\x48\x8B\x01\x44\x38\x89\x1B\x03\x00\x00\x0F\x84",
    "xxx????xxxxxxxx?xxxxx",
    3,
    ZRenderVRDevice**, RenderVRDevice
);
```

Plus the forward declaration in `Include/Globals.h` alongside `ZRenderManager`. During
implementation, confirm whether the slot holds the device directly or the VR manager (the upstream
verified path goes manager `0x03225D20` → device at `+0x141A0`, while the scanned path dereferences
the slot straight to something that passes the device plausibility check). If it is the manager,
declare `ZRenderVRManager` with `ZRenderVRDevice* m_pDevice; // 0x141A0` and expose that instead.

### 3c. `Include/Hooks.h` / `Src/Hooks.cpp`

Two of the three patch groups become detours, which is strictly nicer than byte patching — no
`VirtualProtect`, automatic teardown on mod unload, and other mods can compose with them:

- **Writers A/B** → one `PATTERN_HOOK` on the enclosing function
  (`ZRenderVRDevice_UpdateFoveationState` or similar; pattern anchored on the
  `8B 97 D8 04 00 00 83 FA 01` prologue of the existing signature). The mod detours it, calls the
  original, then clears `m_bWideNarrowOverlay`. Replaces two byte patches.
- **FOV limit ×2** → the method is at vtable slot `+0x208` in both device classes, so either two
  `PATTERN_HOOK`s (one per class, using the two existing 44-byte signatures) or one
  `PATTERN_VTABLE_HOOK` per class vtable. The mod returns `HookAction::Return(1)`. Replaces two byte
  patches.
- **View count** → *cannot* become a hook. The patch site is a `cmpb`/`cmovne` pair mid-function;
  detouring the enclosing function would mean reimplementing its view-count push logic against a
  private render context, which is far more fragile than the 7-byte patch. **This one stays a
  byte patch** via the already-exported `SDK()->PatchCode()`, needing no SDK change.

### 3d. The mod under Option B

```
Mods/VRFoveationFix/
  CMakeLists.txt          (same as Option A)
  Src/VRFoveationFix.h    (plugin + 3 DECLARE_PLUGIN_DETOUR)
  Src/VRFoveationFix.cpp  (~80 lines)
```

`Init()` null-checks `Hooks::ZRenderVRDevice_UpdateFoveationState` etc., adds the detours, and calls
`SDK()->PatchCodeStoreOriginal(...)` once for the view count. `OnFrameUpdate` uses
`(*Globals::RenderVRDevice)->m_aOverlayScaleRatios` and friends — named fields, no offsets. The
destructor reverts the one patch; the SDK removes the detours itself. Settings, state machine, and
logging are unchanged from Option A.

---

## 4. The cost of Option B, stated plainly

- **A failed VR pattern can unload the SDK for everyone.** `PATTERN_HOOK` and
  `PATTERN_RELATIVE_GLOBAL` resolve at static init, whether or not any mod uses them, and each
  failure calls `Fail()` (`ZHMModSDK/Src/HookImpl.h:208`, `Src/GlobalsImpl.h:22`). At
  `g_Failures >= 3` the SDK logs "Too many errors occurred" and unloads
  (`ZHMModSDK/Src/ModSDK.cpp:732`). Adding 3-4 VR patterns to core means a game update that moves
  only the VR renderer can push a flatscreen-only user over that threshold. This is the single
  strongest argument against Option B and would be the first thing raised in review.
- **Someone has to maintain them.** The SDK's patterns get refreshed every game release; VR ones can
  only be validated by a maintainer with a headset and both runtimes.
- **The layout has to be right, not just working.** A `PAD(0x319)`-shaped struct with guessed field
  names in a public header is a maintenance commitment; Option A's local offsets are private to the
  mod and can be wrong-but-harmless.
- **It does not eliminate raw patching anyway** — the view-count site stays a byte patch either way.

What Option B buys: a mod that reads like the rest of `Mods/`, detour-based teardown for 4 of the 5
sites, reusable `Globals::RenderVRDevice` and device types for any future VR mod, and pattern
updates that ride along with the SDK's existing per-release process.

---

## 5. Sequencing

1. Ship **Option A** — self-contained, one-line core diff, provable on one machine.
2. Once the patterns have survived a game update or two and the device field names are confirmed,
   upstream the pieces of **Option B** that are clearly safe (the Glacier types first, then
   `Globals::RenderVRDevice`, then the hooks), deleting mod-local code as each lands.

Doing B first means asking SDK maintainers to accept unproven VR patterns into a failure path that
can unload the SDK for non-VR users.

## 6. Structuring Option A for a cheap migration

Keep the signatures, offsets, and scanner in `CodePatcher.h` plus a single `Layout.h`, and have
`VRFoveationFix.cpp` talk to a thin accessor layer (`Device()`, `ForceTwoLayers()`) that can later be
reimplemented on top of `Globals::` and `Hooks::` without touching the mod's logic.

## 7. Notes carried over from upstream

- Both VR backends share the device layout; the FOV-limit site exists once per backend class and
  both must be handled, or the other runtime keeps a narrow field of view.
- Inert-by-default on failure: ambiguous or missing signature → log, change nothing.
- `README.md` should credit RealChrizzl (MIT) and point at upstream `docs/UPDATING.md` for the
  procedure when a game build moves the sites.
- This writes to the memory of a game with an online connection — same caveat upstream states
  plainly. Worth a line in that README.

## 8. Verification

1. Build and install: `cmake --build _build/x64-Debug --target install`; confirm
   `VRFoveationFix.dll` lands in `<game>/Retail/mods`.
2. Enable it in the in-game Mod Selector (adds `[VRFoveationFix]` to `Retail/mods.ini`), restart.
3. Flatscreen smoke test, no headset: the SDK log shows all six signatures resolving uniquely and
   "code patched"; state sits at `WaitingForVR`; the game renders and plays normally — the patches
   are inert without a VR device.
4. Negative test: corrupt one signature byte in the source, rebuild, confirm the mod names the site,
   applies nothing, and the game is unaffected.
5. VR test on each backend (Oculus/Link and SteamVR): start VR, load a mission, confirm the log
   reaches `Active` and the periphery is sharp; confirm the reported per-eye resolution doubles per
   axis versus stock.
5b. Transparency and refraction, the check §9 exists for and the one that is expensive to skip:
   glass panes, NPC glasses, flowing water, bottles, emissive lights and at least one large panorama
   window, while turning and leaning. Both eyes must agree, and nothing may pop in both eyes at once
   or change brightness with head angle. The diagnostic report's `copies on two views` must be
   climbing and `unexpected counts` must stay at zero.
5c. Save-game load, several times: the periphery must stay sharp and the black centre circle must
   not reappear. `renderer guard` in the report should be running; a few repairs are expected.
6. Unload test: unload the mod from the Mod Selector mid-mission; confirm restore is logged, the
   image returns to stock foveation, and nothing crashes.
7. `VRFoveationFix.ini` → `enabled = false`, restart, confirm nothing is patched.

---

## 9. Catching up with upstream v1.4

Upstream released v1.4 after this port was written. Two real fixes and some lifecycle hardening;
everything that exists only because upstream is an external admin process — code caves, thread
suspension, owner thread-id gating, the WinForms UI — is deliberately not carried over.

Checked against the installed `HITMAN3.exe` before any of it was written: PE timestamp `1781013974`,
which is the verified 3.270.1, and all nine signatures (the five patches, the device locator and the
three new refraction sites) match exactly once each and land on the RVAs upstream documents.

### 9a. The refraction split — `src/RefractionFix.h/.cpp`

Forcing the view count to 4 (§1) is right for geometry, because the view-matrix accessor maps view 2
back to eye 0 and view 3 back to eye 1 with foveation off. `CopyRefractionDepth` is the exception: it
multiplies its fullscreen draw range by the current count, so at 4 the narrow foveal views leak into
glass, water, bottles, NPC glasses and some emissive lights — different content per eye, plus
angle-dependent pop-in. It has to see 2 while everything around it still sees 4.

Upstream splices hand-assembled wrappers into the two call sites, with an owner scope, thread-id
gating and telemetry counters, because from another process that is the only way to scope the change.
In process this collapses to two detours, and the binary says why it can:

- `CopyRefractionDepth` (RVA `0x128FE20`) has **exactly two callers**, and they are the two sites
  upstream wraps.
- Both of them are inside `DrawRefractiveAndTransparent` (RVA `0x1290220`), which itself has
  **exactly one caller**.

So the detour on the copy is already as narrowly scoped as upstream's call-site wrappers. The only
thing the outer detour is needed for is the render context: it is that pass's first argument, and it
is not an argument of the copy. It goes into a `thread_local` for the duration of the pass, which is
what upstream's owner scope amounts to once the tid and re-entrancy checks are unnecessary.

Argument counts have to be exact, because a detour passes them through: the pass takes **10**
integer/pointer arguments (no floats — its one call site writes `[rsp+0x20]` … `[rsp+0x48]` and no
xmm), the copy takes **6**. The call targets are decoded from the `E8` displacement of each located
site; both copy sites must resolve to the same function or the mod refuses.

Hooking needs a detour at an arbitrary address, which the SDK does not expose — `Hooks` is a fixed
exported set — so the mod links its own copy of MinHook, the same library the SDK uses. That drags in
`src/MsvcIntrinsics.cpp`: MinHook's trampoline builder calls `__movsb`, which clang-cl leaves
undefined when cross-compiling; the SDK carries the same shim for the same reason.

Detours also mean removal is clean. Upstream has to suspend every game thread and check each
instruction pointer before it can take its wrappers out; here MinHook does it, and the scope detour
is removed first so the copy detour is already a pass-through when it goes.

### 9b. Lifecycle hardening — `VRFoveationFix::SyncDeviceFields`

One validated write path, taken by both the frame update and the guard, under one lock:

- **Validate before capturing.** The device is built field by field, so the scale and mask blocks are
  readable before they hold anything. Writing then is harmless; *remembering* an all-zero block as
  the stock values is not — it would be restored on unload. Both blocks must contain plausible floats
  (`0.05 … 20.0`, `-0.01 … 4.0`, upstream's bounds) before anything is touched.
- **Write before `+0x319` says active.** The render state can reach a mission ahead of that flag, so
  the fields are written as soon as the geometry is plausible.
- **Read back, roll back.** A write is not a result: the values are read back, and if they did not
  stick, what was there is put back, the snapshot is dropped if this was the first write to that
  device, and nothing reports success. `Active` requires a confirmed write.
- **Forget on device change.** A new device object means the remembered stock values belong to
  something that no longer exists.

### 9c. The renderer guard

The game restores its own foveation values during mission, scene and save-game loads, inside a window
upstream measures at up to 15 ms — and upstream's Linux notes say Proton beats their 15 ms loop
outright, which is exactly this machine. A frame update samples at ~11 ms at 90 fps and worse during
a load, so a guard thread re-checks the 24 bytes that matter about once a millisecond, writing nothing
while they are right, and going through §9b when they are not. It starts after the first confirmed
write and resolves the device pointer from the game's own global every pass, so a device torn down
between two passes is never written to.

### 9d. Not taken

Upstream's transition-3 latch (`NeedsMissionReload`) — the guard closes the window it reported on.
Also skipped: the `Mesh`, `WaterGuard` and `SpriteGuard` sites, which upstream ships as stock-only
guards and are not part of the fix.
