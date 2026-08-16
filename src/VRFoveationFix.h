#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <IPluginInterface.h>

#include "CodePatcher.h"
#include "RefractionFix.h"

struct SGameUpdateEvent;

/**
 * Removes HITMAN's fixed foveation (Wide/Narrow Overlay) in PC VR, so the whole field of view is
 * rendered at full resolution instead of a sharp circle in the centre and a blurry periphery.
 *
 * Port of https://github.com/RealChrizzl/hitman-vr-foveation-fix (MIT, by RealChrizzl).
 * See Layout.h for the signatures and device offsets, and PLAN.md for what of this mod would be
 * replaced by SDK core support.
 */
class VRFoveationFix : public IPluginInterface {
public:
    ~VRFoveationFix() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;

private:
    enum class State {
        /** Disabled by settings, or a signature could not be located. Nothing was changed. */
        Inert,

        /** The code is patched, waiting for VR to come up. */
        WaitingForVR,

        /** VR is running, waiting for a mission to load. */
        WaitingForMission,

        /** The fix is in effect. */
        Active,

        /** Something went wrong after patching. */
        Failed,
    };

    /** What a pass over the device fields did. */
    enum class SyncResult {
        /** The device is not built far enough to be touched. Nothing was read back or written. */
        NotReady,

        /** The fields hold the values this mod wants, confirmed by reading them back. */
        Fixed,

        /** A write did not stick. Whatever could be put back was put back. */
        Failed,
    };

    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    /** Locates every signature. Returns false without changing anything if any is not unique. */
    bool LocateSites(std::vector<VRFov::MemoryPatch>& p_OutPatches);

    /** Returns the VR device if the pointer currently looks like one, otherwise nullptr. */
    uintptr_t GetDevice() const;

    /**
     * The one place the device fields are written. Validates the fields, remembers the stock ones
     * the first time it touches a device, writes, and reads back to confirm. Both the frame update
     * and the guard thread come through here, so it takes the lock itself.
     */
    SyncResult SyncDeviceFields(uintptr_t p_Device);

    /** Writes the remembered stock device field values back. */
    void RestoreDeviceFields();

    /** Forgets what was remembered about a device. The caller holds the lock. */
    void ForgetDeviceFieldsLocked();

    /**
     * Starts the guard, once, after the first confirmed write. It re-checks the 24 bytes that
     * matter about once a millisecond, because the game restores its own values during save and
     * mission loads faster than a frame update can catch - measurably so under Proton.
     */
    void StartGuard();
    void StopGuard();
    void GuardLoop();

    /** Logs only when the state actually changes. */
    void SetState(State p_State, const char* p_Message);

    /**
     * Dumps everything this mod knows about the running game to the console: the build, the VR
     * runtime modules, where every signature landed and the current contents of the VR device.
     * Reads only. The same report as the upstream project's HitmanVRProbe, so it can be pasted
     * into an issue when the fix does not work on a build or a backend.
     */
    void LogDiagnostics() const;

    static const char* StateName(State p_State);

    std::vector<VRFov::MemoryPatch> m_Patches;
    VRFov::RefractionFix m_Refraction;
    bool m_Patched = false;
    bool m_FrameUpdateRegistered = false;

    /** Address of the global holding the VR device pointer, from the locator signature. */
    uintptr_t m_DeviceSlot = 0;

    /** Offset of the Wide/Narrow Overlay flag inside the device, from the locator signature. */
    uint32_t m_WnoOffset = 0;

    /** Guards the device fields below, and the writes to the device itself. */
    mutable std::mutex m_DeviceLock;

    /** Device the fields were last written to, so they can be written back on unload. */
    uintptr_t m_Device = 0;

    bool m_DeviceFieldsSaved = false;
    float m_StockScaleRatios[4] = {};
    float m_StockOverlayMask[2] = {};

    std::thread m_Guard;

    /** Manual reset event, signalled to stop the guard. A HANDLE, kept opaque to this header. */
    void* m_GuardStop = nullptr;

    /** Times the guard found the fields changed back and repaired them. */
    std::atomic<uint64_t> m_GuardRepairs = 0;

    State m_State = State::Inert;
};

DECLARE_ZHM_PLUGIN(VRFoveationFix)
