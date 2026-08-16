#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Everything this mod knows about the game's VR renderer: the code signatures it patches and the
 * layout of the VR device object. Kept in one file so a game update only ever needs changes here.
 *
 * Signatures and offsets are taken from https://github.com/RealChrizzl/hitman-vr-foveation-fix
 * (MIT, by RealChrizzl), verified against build 3.270.1. See that project's docs/HOW-IT-WORKS.md for
 * what each site does and docs/UPDATING.md for how to refresh them; PLAN.md covers what of this file
 * would move into SDK core.
 */
namespace VRFov::Layout {
    /** A code site to patch, described the way the upstream project describes it. */
    struct Signature {
        /** Human readable name, used in log messages. */
        const char* Name;

        /** Space separated hex bytes, "??" for a wildcard. Must match exactly once. */
        const char* Pattern;

        /** Offset from the start of the match to the first byte to overwrite. */
        size_t PatchOffset;

        /** Replacement bytes. */
        const uint8_t* Fix;

        /** Number of replacement bytes. */
        size_t FixSize;
    };

    // The pattern includes the stock bytes at the patch site, so a successful match is also proof
    // that the site has not already been patched by something else.

    inline constexpr uint8_t FixWnoWriterA[] = {0xB1, 0x00, 0x90};
    inline constexpr uint8_t FixWnoWriterB[] = {0xB0, 0x00, 0x90};
    inline constexpr uint8_t FixFovLimit[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90};
    inline constexpr uint8_t FixViewCount[] = {0x48, 0x85, 0xE4, 0x90, 0x90, 0x90, 0x90};

    inline constexpr Signature CodeSites[] = {
        {
            "two layers instead of four (writer A)",
            "8B 97 D8 04 00 00 83 FA 01 0F 94 C1 88 8F 1B 03 00 00",
            9, FixWnoWriterA, sizeof(FixWnoWriterA)
        },
        {
            "two layers instead of four (writer B)",
            "8B 97 D8 04 00 00 83 FA 01 0F 94 C0 88 87 1B 03 00 00",
            9, FixWnoWriterB, sizeof(FixWnoWriterB)
        },
        {
            "full field of view, Oculus device",
            "C0 08 00 00 45 33 C0 4C 8B 8E C8 7A 00 00 48 8B D3 48 89 6C 24 28 48 89 6C 24 20 "
            "48 8B 01 FF 50 28 48 8B CB E8 ?? ?? ?? ?? FF 4B 14 0F B6 87 1B 03 00 00",
            44, FixFovLimit, sizeof(FixFovLimit)
        },
        {
            "full field of view, OpenVR device",
            "50 09 00 00 45 33 C0 4C 8B 8E C8 7A 00 00 48 8B D3 48 89 6C 24 28 48 89 6C 24 20 "
            "48 8B 01 FF 50 28 48 8B CB E8 ?? ?? ?? ?? FF 4B 14 0F B6 87 1B 03 00 00",
            44, FixFovLimit, sizeof(FixFovLimit)
        },
        {
            "view count 4 - without this, geometry disappears",
            "74 16 49 8B 85 A0 41 01 00 41 8B CF 80 B8 1B 03 00 00 00 0F 45 CF",
            12, FixViewCount, sizeof(FixViewCount)
        },
    };

    // --- The refraction split (upstream v1.4) --------------------------------------------------
    // Forcing the view count to 4 above is right for geometry, because the view matrix accessor
    // maps view 2 back to eye 0 and view 3 back to eye 1 when foveation is off. The refraction
    // depth copy is the exception: it multiplies its fullscreen draw range by the count, so with 4
    // it renders the two narrow foveal views into glass, water, bottles and some emissive lights,
    // differently in each eye. It has to see 2 while everything around it still sees 4.
    //
    // Neither site is patched. The call target is decoded out of each match and detoured, so the
    // count is swapped for the duration of the copy and put back afterwards.

    /** A call whose target this mod detours. */
    struct CallSite {
        /** Human readable name, used in log messages. */
        const char* Name;

        /** Space separated hex bytes, "??" for a wildcard. Must match exactly once. */
        const char* Pattern;

        /** Offset from the start of the match to the `E8` of the call. */
        size_t CallOffset;
    };

    /**
     * The call to the pass that draws refractive and transparent geometry. Detoured only to learn
     * which render context the pass is running on: that is its first argument.
     */
    inline constexpr CallSite RefractionScopeCall = {
        "the refractive and transparent pass",
        "48 8B 8C 24 C0 00 00 00 48 89 44 24 20 E8 ?? ?? ?? ?? 48 8D 8C 24 60 02 00 00",
        13
    };

    /**
     * The two calls to the refraction depth copy. Both resolve to the same function, which is
     * checked, and that function has no other callers in the game.
     */
    inline constexpr CallSite RefractionCopyCalls[] = {
        {
            "refraction depth copy A",
            "48 8B 8D B0 01 00 00 48 8D 95 D0 01 00 00 89 44 24 28 4D 8B C4 8B 41 04 44 8B 09 "
            "48 8B CE 89 44 24 20 E8 ?? ?? ?? ?? 48 8B 9D D0 01 00 00 48 8B F8 48 8B 0D ?? ?? ?? ?? "
            "4C 8D 0D ?? ?? ?? ??",
            34
        },
        {
            "refraction depth copy B",
            "48 8B 8D B0 01 00 00 48 8D 95 D0 01 00 00 89 44 24 28 4D 8B C4 8B 41 04 44 8B 09 "
            "48 8B CE 89 44 24 20 E8 ?? ?? ?? ?? 48 8B 9D D0 01 00 00 48 8B F8 48 8B 85 B0 01 00 00 "
            "4C 8B 40 60 4D 85 C0",
            34
        },
    };

    /**
     * The render context keeps a stack of view counts: uint32 entries from offset 0, with the
     * index of the current one here.
     */
    inline constexpr ptrdiff_t ContextCountTop = 0x14;

    /** Largest index that still lies inside the count stack. Anything above means it is not one. */
    inline constexpr uint32_t MaxContextCountTop = 4;

    /** The count geometry and visibility need, and the one the refraction depth copy needs. */
    inline constexpr uint32_t ViewCountLogical = 4;
    inline constexpr uint32_t ViewCountPhysical = 2;

    /**
     * Locator for the global that holds the VR device pointer. Never patched.
     * `mov rcx, [rip+disp]` at +0 gives the global; the `cmp byte [rcx+0x31B]` later in the match
     * gives the offset of the Wide/Narrow Overlay flag inside the device.
     */
    inline constexpr const char* DeviceLocatorPattern =
            "48 8B 0D ?? ?? ?? ?? 8B D6 48 8B 01 44 38 B9 1B 03 00 00 0F 84";

    /** Offset of the rel32 displacement of the `mov rcx, [rip+disp]` in the locator match. */
    inline constexpr size_t DeviceLocatorRelOffset = 3;

    /** Offset of the uint32 displacement that encodes the WNO flag's offset in the device. */
    inline constexpr size_t DeviceLocatorWnoOffset = 15;

    // --- VR device field offsets ---------------------------------------------------------------
    // The two backends (Oculus, OpenVR) share this layout; verified on both upstream.

    /** uint32 requested and cached VR mode. Diagnostics only. */
    inline constexpr ptrdiff_t DeviceMode = 0x220;
    inline constexpr ptrdiff_t DeviceCachedMode = 0x30C;

    /** bool, 1 once VR is actually running. */
    inline constexpr ptrdiff_t DeviceActive = 0x319;

    /** float[4] field of view tangents. Only read, as a plausibility check on the pointer. */
    inline constexpr ptrdiff_t DeviceFovTangents = 0x420;

    /** float[4] small/large scale ratios. Forced to 1.0 to neutralise the overlay. */
    inline constexpr ptrdiff_t DeviceScaleRatios = 0x490;

    /** Two floats: overlay pass blend (0x4C0) and centre circle radius (0x4C4). Forced to 0. */
    inline constexpr ptrdiff_t DeviceOverlayMask = 0x4C0;

    /** uint32 transition state. 3 means a mission is up and rendering. */
    inline constexpr ptrdiff_t DeviceTransition = 0x4D8;

    /** uint32 per-eye render width / height. Logged only. */
    inline constexpr ptrdiff_t DeviceWidth = 0x510;
    inline constexpr ptrdiff_t DeviceHeight = 0x514;

    /** uint16 layer count. 2 once the fix is in effect, 4 in stock foveated mode. */
    inline constexpr ptrdiff_t DeviceLayerCount = 0x520;

    /** Pointer to the render texture. Changes when the render target is recreated. */
    inline constexpr ptrdiff_t DeviceTexture = 0x530;

    /** Pointer to the render view. Diagnostics only. */
    inline constexpr ptrdiff_t DeviceView = 0x538;

    /** Largest field offset touched above, used to bounds check the device pointer. */
    inline constexpr size_t DeviceMinSize = 0x538;

    /** Past the last field the diagnostics dump reads. */
    inline constexpr size_t DeviceDiagnosticSize = 0x540;

    /** PE timestamp of the build the signatures and offsets above were verified against (3.270.1). */
    inline constexpr uint32_t VerifiedTimestamp = 1781013974;

    /** Upper bound for a plausible WNO flag offset decoded from the locator. */
    inline constexpr uint32_t MaxPlausibleFieldOffset = 0x4000;

    /** The four scale ratios, neutralised. */
    inline constexpr float ScaleRatiosFix[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    /** Overlay pass and centre circle, both off. */
    inline constexpr float OverlayMaskFix[2] = {0.0f, 0.0f};

    /** Field of view tangents outside this range mean the pointer is not a VR device. */
    inline constexpr float MinFovTangent = 0.2f;
    inline constexpr float MaxFovTangent = 3.0f;

    // The device is built field by field, so the scale and overlay blocks are readable before they
    // hold anything. Writing then would be harmless, but *remembering* what was there would not:
    // a half built block, all zeroes, would be restored as the stock values on unload. Both blocks
    // are therefore only touched once they contain plausible floats. Same bounds as upstream.

    /** Scale ratios outside this range mean the device builder has not filled them in yet. */
    inline constexpr float MinScaleRatio = 0.05f;
    inline constexpr float MaxScaleRatio = 20.0f;

    /** Same for the overlay pass blend and centre circle radius. */
    inline constexpr float MinOverlayMask = -0.01f;
    inline constexpr float MaxOverlayMask = 4.0f;

    /** Value of DeviceTransition that means a mission is loaded and rendering. */
    inline constexpr uint32_t TransitionInMission = 3;
}
