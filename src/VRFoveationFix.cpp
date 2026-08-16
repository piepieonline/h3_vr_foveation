#include "VRFoveationFix.h"

#include <array>
#include <cmath>
#include <cstring>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <imgui.h>
#include <IconsMaterialDesign.h>

#include <Globals.h>
#include <Logging.h>

#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZGameLoopManager.h>

#include "Layout.h"
#include "ProcessInfo.h"

using namespace VRFov;

namespace {
    /** Reads a field out of the VR device. The device pointer is validated before any of these. */
    template <class T>
    T ReadField(uintptr_t p_Device, ptrdiff_t p_Offset) {
        T s_Value;
        memcpy(&s_Value, reinterpret_cast<const void*>(p_Device + p_Offset), sizeof(T));
        return s_Value;
    }

    template <class T>
    void WriteField(uintptr_t p_Device, ptrdiff_t p_Offset, const T& p_Value) {
        memcpy(reinterpret_cast<void*>(p_Device + p_Offset), &p_Value, sizeof(T));
    }

    /** True if every value is a real number inside the range. */
    template <size_t N>
    bool Plausible(const std::array<float, N>& p_Values, float p_Min, float p_Max) {
        for (const auto s_Value : p_Values) {
            if (!std::isfinite(s_Value) || s_Value < p_Min || s_Value > p_Max) {
                return false;
            }
        }

        return true;
    }

    /** "1, 1, 1, 1" for the diagnostics dump. */
    template <size_t N>
    std::string FormatFloats(const std::array<float, N>& p_Values) {
        std::string s_Result;

        for (const auto s_Value : p_Values) {
            if (!s_Result.empty()) {
                s_Result += ", ";
            }

            s_Result += fmt::format("{:g}", s_Value);
        }

        return s_Result;
    }

    /** The whole span of device fields this mod writes, for one readability check. */
    constexpr size_t c_WrittenFieldSpan =
            Layout::DeviceOverlayMask + sizeof(Layout::OverlayMaskFix) - Layout::DeviceScaleRatios;
}

VRFoveationFix::~VRFoveationFix() {
    if (m_FrameUpdateRegistered && Globals::GameLoopManager) {
        const ZMemberDelegate<VRFoveationFix, void(const SGameUpdateEvent&)> s_Delegate(
            this, &VRFoveationFix::OnFrameUpdate
        );
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
        m_FrameUpdateRegistered = false;
    }

    // Before the restore below, or the guard would write the fix straight back over it.
    StopGuard();

    RestoreDeviceFields();

    m_Refraction.Uninstall();

    for (auto& s_Patch : m_Patches) {
        s_Patch.Revert();
    }

    if (m_Patched) {
        Logger::Info("[VRFoveationFix] Restored the game's original foveated rendering.");
    }

    m_Patched = false;
}

void VRFoveationFix::Init() {
    if (!HasSetting("general", "enabled")) {
        SetSettingBool("general", "enabled", true);
    }

    if (!GetSettingBool("general", "enabled", true)) {
        SetState(State::Inert, "Disabled in VRFoveationFix.ini, nothing was patched.");
        return;
    }

    std::vector<MemoryPatch> s_Patches;

    if (!LocateSites(s_Patches)) {
        SetState(
            State::Inert,
            "Nothing was changed. The game was probably updated - see the project page for how to refresh the signatures."
        );
        return;
    }

    // Patching after VR has already initialised does not work and leaves the renderer in a state
    // the game does not expect, so refuse. This happens when the mod is loaded from the Mod
    // Selector mid-session rather than at startup.
    if (const auto s_Device = GetDevice(); s_Device != 0 &&
        ReadField<uint8_t>(s_Device, Layout::DeviceActive) == 1) {
        SetState(
            State::Inert,
            "VR was already running when this mod loaded. Restart the game with the mod enabled."
        );
        return;
    }

    for (auto& s_Patch : s_Patches) {
        if (s_Patch.Apply()) {
            continue;
        }

        // All or nothing: a partially patched renderer is worse than an unpatched one.
        for (auto& s_Applied : s_Patches) {
            s_Applied.Revert();
        }

        SetState(State::Inert, "A patch could not be applied. Nothing was changed.");
        return;
    }

    // The two layer renderer without this is the state that makes glass, water and some lights
    // disagree between the eyes, so a build this half fits is a build to leave alone entirely.
    if (!m_Refraction.Install()) {
        for (auto& s_Applied : s_Patches) {
            s_Applied.Revert();
        }

        SetState(
            State::Inert,
            "The refraction pass could not be located, so nothing was changed. The game was probably updated."
        );
        return;
    }

    m_Patches = std::move(s_Patches);
    m_Patched = true;

    SetState(State::WaitingForVR, "Patched. Start VR and load a mission.");
}

void VRFoveationFix::OnEngineInitialized() {
    if (!m_Patched || m_FrameUpdateRegistered) {
        return;
    }

    const ZMemberDelegate<VRFoveationFix, void(const SGameUpdateEvent&)> s_Delegate(
        this, &VRFoveationFix::OnFrameUpdate
    );
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);

    m_FrameUpdateRegistered = true;
}

void VRFoveationFix::OnDrawMenu() {
    if (ImGui::Button(ICON_MD_BUG_REPORT " VR FOVEATION")) {
        LogDiagnostics();
    }
}

bool VRFoveationFix::LocateSites(std::vector<MemoryPatch>& p_OutPatches) {
    for (const auto& s_Signature : Layout::CodeSites) {
        const auto s_Scan = ScanCode(s_Signature.Pattern);

        if (!s_Scan.IsUnique()) {
            Logger::Error(
                "[VRFoveationFix] The code for '{}' could not be located uniquely in this build ({} matches).",
                s_Signature.Name, s_Scan.HitCount
            );
            return false;
        }

        p_OutPatches.emplace_back(
            s_Signature.Name, s_Scan.Address + s_Signature.PatchOffset, s_Signature.Fix, s_Signature.FixSize
        );
    }

    const auto s_Locator = ScanCode(Layout::DeviceLocatorPattern);

    if (!s_Locator.IsUnique()) {
        Logger::Error(
            "[VRFoveationFix] The VR device reference could not be located uniquely in this build ({} matches).",
            s_Locator.HitCount
        );
        p_OutPatches.clear();
        return false;
    }

    // mov rcx, [rip+disp32] - the target is relative to the end of the instruction.
    const auto s_RelAddress = s_Locator.Address + Layout::DeviceLocatorRelOffset;
    const auto s_Displacement = ReadField<int32_t>(s_RelAddress, 0);

    m_DeviceSlot = s_RelAddress + s_Displacement + sizeof(int32_t);
    m_WnoOffset = ReadField<uint32_t>(s_Locator.Address + Layout::DeviceLocatorWnoOffset, 0);

    if (m_WnoOffset == 0 || m_WnoOffset > Layout::MaxPlausibleFieldOffset) {
        Logger::Error("[VRFoveationFix] Implausible device layout in this build (WNO flag at {:#x}).", m_WnoOffset);
        p_OutPatches.clear();
        m_DeviceSlot = 0;
        return false;
    }

    Logger::Debug(
        "[VRFoveationFix] Device pointer at {}, WNO flag at device+{:#x}.",
        fmt::ptr(reinterpret_cast<void*>(m_DeviceSlot)), m_WnoOffset
    );

    return true;
}

uintptr_t VRFoveationFix::GetDevice() const {
    if (m_DeviceSlot == 0 || !IsReadable(reinterpret_cast<const void*>(m_DeviceSlot), sizeof(uintptr_t))) {
        return 0;
    }

    const auto s_Device = ReadField<uintptr_t>(m_DeviceSlot, 0);

    if (!IsReadable(reinterpret_cast<const void*>(s_Device), Layout::DeviceMinSize)) {
        return 0;
    }

    // The slot is populated before it holds anything meaningful, so check the shape of the object:
    // four field of view tangents in a sane range and an active flag that is a boolean.
    if (!Plausible(
        ReadField<std::array<float, 4>>(s_Device, Layout::DeviceFovTangents),
        Layout::MinFovTangent, Layout::MaxFovTangent
    )) {
        return 0;
    }

    if (ReadField<uint8_t>(s_Device, Layout::DeviceActive) > 1) {
        return 0;
    }

    return s_Device;
}

VRFoveationFix::SyncResult VRFoveationFix::SyncDeviceFields(uintptr_t p_Device) {
    std::lock_guard s_Lock(m_DeviceLock);

    if (!IsReadable(reinterpret_cast<const void*>(p_Device + Layout::DeviceScaleRatios), c_WrittenFieldSpan)) {
        return SyncResult::NotReady;
    }

    const auto s_ScaleRatios = ReadField<std::array<float, 4>>(p_Device, Layout::DeviceScaleRatios);
    const auto s_OverlayMask = ReadField<std::array<float, 2>>(p_Device, Layout::DeviceOverlayMask);

    // The device is built field by field. Writing into a half built block would be harmless, but
    // remembering it as the stock values would not, so wait until both blocks look like floats.
    if (!Plausible(s_ScaleRatios, Layout::MinScaleRatio, Layout::MaxScaleRatio) ||
        !Plausible(s_OverlayMask, Layout::MinOverlayMask, Layout::MaxOverlayMask)) {
        return SyncResult::NotReady;
    }

    // A different device object: what was remembered belongs to the old one and is of no use.
    if (m_Device != 0 && m_Device != p_Device) {
        ForgetDeviceFieldsLocked();
    }

    m_Device = p_Device;

    const auto s_Matches = [](const auto& p_Values, const auto& p_Wanted) {
        return memcmp(p_Values.data(), p_Wanted, sizeof(p_Wanted)) == 0;
    };

    if (s_Matches(s_ScaleRatios, Layout::ScaleRatiosFix) && s_Matches(s_OverlayMask, Layout::OverlayMaskFix)) {
        return SyncResult::Fixed;
    }

    const auto s_WasSaved = m_DeviceFieldsSaved;

    if (!s_WasSaved) {
        memcpy(m_StockScaleRatios, s_ScaleRatios.data(), sizeof(m_StockScaleRatios));
        memcpy(m_StockOverlayMask, s_OverlayMask.data(), sizeof(m_StockOverlayMask));
        m_DeviceFieldsSaved = true;
    }

    WriteField(p_Device, Layout::DeviceScaleRatios, Layout::ScaleRatiosFix);
    WriteField(p_Device, Layout::DeviceOverlayMask, Layout::OverlayMaskFix);

    // The game can rebuild this state underneath the write, so a write is not a result. Read it
    // back before anything reports success.
    const auto s_AfterScale = ReadField<std::array<float, 4>>(p_Device, Layout::DeviceScaleRatios);
    const auto s_AfterMask = ReadField<std::array<float, 2>>(p_Device, Layout::DeviceOverlayMask);

    if (s_Matches(s_AfterScale, Layout::ScaleRatiosFix) && s_Matches(s_AfterMask, Layout::OverlayMaskFix)) {
        return SyncResult::Fixed;
    }

    // Put back what was there and, if this was the first write to this device, forget it again -
    // the values captured a moment ago may have been mid-rebuild rather than the stock ones.
    WriteField(p_Device, Layout::DeviceScaleRatios, s_ScaleRatios);
    WriteField(p_Device, Layout::DeviceOverlayMask, s_OverlayMask);

    if (!s_WasSaved) {
        ForgetDeviceFieldsLocked();
        m_Device = p_Device;
    }

    return SyncResult::Failed;
}

void VRFoveationFix::ForgetDeviceFieldsLocked() {
    m_DeviceFieldsSaved = false;
    m_Device = 0;
}

void VRFoveationFix::RestoreDeviceFields() {
    std::lock_guard s_Lock(m_DeviceLock);

    if (!m_DeviceFieldsSaved || m_Device == 0) {
        return;
    }

    if (IsReadable(reinterpret_cast<const void*>(m_Device + Layout::DeviceScaleRatios), c_WrittenFieldSpan)) {
        WriteField(m_Device, Layout::DeviceScaleRatios, m_StockScaleRatios);
        WriteField(m_Device, Layout::DeviceOverlayMask, m_StockOverlayMask);
    }

    ForgetDeviceFieldsLocked();
}

void VRFoveationFix::StartGuard() {
    // Only ever called from the frame update, so no lock is needed around the thread itself.
    if (m_Guard.joinable()) {
        return;
    }

    m_GuardStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (m_GuardStop == nullptr) {
        Logger::Error("[VRFoveationFix] The renderer guard could not be started; save loads may bring the blur back.");
        return;
    }

    m_Guard = std::thread(&VRFoveationFix::GuardLoop, this);

    Logger::Debug("[VRFoveationFix] Renderer guard running.");
}

void VRFoveationFix::StopGuard() {
    if (m_Guard.joinable()) {
        SetEvent(static_cast<HANDLE>(m_GuardStop));
        m_Guard.join();
    }

    if (m_GuardStop != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_GuardStop));
        m_GuardStop = nullptr;
    }
}

void VRFoveationFix::GuardLoop() {
    // A millisecond, as upstream's guard uses. How close it gets depends on the process timer
    // resolution, which the game already raises for VR. Waiting on the event rather than sleeping
    // means shutting the guard down is immediate.
    while (WaitForSingleObject(static_cast<HANDLE>(m_GuardStop), 1) == WAIT_TIMEOUT) {
        // Resolved from the game's own pointer every time rather than remembered, so a device
        // that has been torn down between two passes is never written to.
        const auto s_Device = GetDevice();

        if (s_Device == 0) {
            continue;
        }

        // The fast path reads the two blocks and nothing else, and writes nothing at all while
        // they are right - which is almost always.
        if (!IsReadable(
            reinterpret_cast<const void*>(s_Device + Layout::DeviceScaleRatios), c_WrittenFieldSpan
        )) {
            continue;
        }

        const auto s_ScaleRatios = ReadField<std::array<float, 4>>(s_Device, Layout::DeviceScaleRatios);
        const auto s_OverlayMask = ReadField<std::array<float, 2>>(s_Device, Layout::DeviceOverlayMask);

        if (memcmp(s_ScaleRatios.data(), Layout::ScaleRatiosFix, sizeof(Layout::ScaleRatiosFix)) == 0 &&
            memcmp(s_OverlayMask.data(), Layout::OverlayMaskFix, sizeof(Layout::OverlayMaskFix)) == 0) {
            continue;
        }

        // Everything past here is the same validated write the frame update does, under the same
        // lock, so the two can never write at once.
        if (SyncDeviceFields(s_Device) == SyncResult::Fixed) {
            m_GuardRepairs.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void VRFoveationFix::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    if (!m_Patched) {
        return;
    }

    const auto s_Device = GetDevice();

    if (s_Device == 0) {
        SetState(State::WaitingForVR, "Waiting for VR to start.");
        return;
    }

    // Written as soon as the device holds plausible geometry, which is before it reports itself
    // active: during a load the render state can reach a mission ahead of that flag.
    const auto s_Sync = SyncDeviceFields(s_Device);

    // Only once there is something worth guarding, as upstream does.
    if (s_Sync == SyncResult::Fixed) {
        StartGuard();
    }

    if (ReadField<uint8_t>(s_Device, Layout::DeviceActive) != 1) {
        SetState(State::WaitingForVR, "Waiting for VR to start.");
        return;
    }

    if (ReadField<uint8_t>(s_Device, m_WnoOffset) != 0) {
        SetState(
            State::Failed,
            "VR started before the patch took effect. Restart the game with the mod enabled."
        );
        return;
    }

    if (s_Sync == SyncResult::Failed) {
        SetState(
            State::Failed,
            "The renderer values would not hold. The game may be reloading; this retries every frame."
        );
        return;
    }

    const auto s_Transition = ReadField<uint32_t>(s_Device, Layout::DeviceTransition);
    const auto s_LayerCount = ReadField<uint16_t>(s_Device, Layout::DeviceLayerCount);
    const auto s_Texture = ReadField<void*>(s_Device, Layout::DeviceTexture);

    // Menus and loading screens pass through here on the way to a mission, and back out of one.
    if (s_Sync != SyncResult::Fixed || s_Transition != Layout::TransitionInMission ||
        s_LayerCount != 2 || s_Texture == nullptr) {
        SetState(State::WaitingForMission, "VR is running in two-layer mode. Load a mission.");
        return;
    }

    if (m_State != State::Active) {
        Logger::Info(
            "[VRFoveationFix] Active. Rendering {}x{} per eye in two layers instead of four.",
            ReadField<uint32_t>(s_Device, Layout::DeviceWidth),
            ReadField<uint32_t>(s_Device, Layout::DeviceHeight)
        );
        m_State = State::Active;
    }
}

void VRFoveationFix::SetState(State p_State, const char* p_Message) {
    if (m_State == p_State) {
        return;
    }

    m_State = p_State;

    if (p_State == State::Failed) {
        Logger::Error("[VRFoveationFix] {}", p_Message);
    }
    else {
        Logger::Info("[VRFoveationFix] {}", p_Message);
    }
}

const char* VRFoveationFix::StateName(State p_State) {
    switch (p_State) {
        case State::Inert:
            return "inert, nothing was patched";
        case State::WaitingForVR:
            return "patched, waiting for VR";
        case State::WaitingForMission:
            return "VR running, waiting for a mission";
        case State::Active:
            return "active";
        case State::Failed:
            return "failed";
    }

    return "unknown";
}

void VRFoveationFix::LogDiagnostics() const {
    const auto s_Base = GetModuleBase();
    const auto s_Timestamp = GetImageTimestamp();

    Logger::Info("[VRFoveationFix] --- diagnostics, nothing is written ---");
    Logger::Info("[VRFoveationFix] state                : {}", StateName(m_State));
    Logger::Info(
        "[VRFoveationFix] game build timestamp : {} ({})", s_Timestamp,
        s_Timestamp == Layout::VerifiedTimestamp ? "the verified build, 3.270.1" : "NOT the verified build"
    );
    Logger::Info("[VRFoveationFix] image base           : {:#x}", s_Base);

    std::string s_Modules;

    for (const auto& s_Module : GetLoadedVrModules()) {
        if (!s_Modules.empty()) {
            s_Modules += ", ";
        }

        s_Modules += s_Module;
    }

    if (s_Modules.empty()) {
        s_Modules = "none found";
    }

    Logger::Info("[VRFoveationFix] VR modules loaded    : {}", s_Modules);

    // Code sites. Once patched the stock bytes are gone, so a rescan would report no match - the
    // patch records are the truth in that case.
    Logger::Info("[VRFoveationFix] code sites:");

    for (size_t i = 0; i < std::size(Layout::CodeSites); ++i) {
        const auto& s_Site = Layout::CodeSites[i];

        if (i < m_Patches.size()) {
            Logger::Info(
                "[VRFoveationFix]   {} : RVA {:#x}, {}", s_Site.Name, m_Patches[i].Address() - s_Base,
                m_Patches[i].IsApplied() ? "patched" : "not patched"
            );
            continue;
        }

        const auto s_Scan = ScanCode(s_Site.Pattern);

        if (s_Scan.IsUnique()) {
            Logger::Info(
                "[VRFoveationFix]   {} : found, patch site at RVA {:#x}", s_Site.Name,
                s_Scan.Address + s_Site.PatchOffset - s_Base
            );
        }
        else {
            Logger::Info(
                "[VRFoveationFix]   {} : NOT USABLE, {}", s_Site.Name,
                s_Scan.HitCount > 1 ? "more than one match" : "no match"
            );
        }
    }

    Logger::Info("[VRFoveationFix] refraction split:");

    if (m_Refraction.IsInstalled()) {
        Logger::Info(
            "[VRFoveationFix]   pass at RVA {:#x}, depth copy at RVA {:#x}, both detoured",
            m_Refraction.ScopeFunction() - s_Base, m_Refraction.CopyFunction() - s_Base
        );
        Logger::Info(
            "[VRFoveationFix]   copies on two views {}, left alone {}, unexpected counts {}",
            RefractionFix::SwapCount(), RefractionFix::PassThroughCount(), RefractionFix::AnomalyCount()
        );
    }
    else {
        Logger::Info("[VRFoveationFix]   not installed - glass and water would differ between the eyes");
    }

    Logger::Info(
        "[VRFoveationFix] renderer guard       : {}, {} repairs", m_Guard.joinable() ? "running" : "not running",
        m_GuardRepairs.load(std::memory_order_relaxed)
    );

    auto s_Slot = m_DeviceSlot;
    auto s_WnoOffset = m_WnoOffset;

    // When the mod is inert nothing has been resolved, so resolve it here for the report only.
    if (s_Slot == 0) {
        if (const auto s_Locator = ScanCode(Layout::DeviceLocatorPattern); s_Locator.IsUnique()) {
            const auto s_RelAddress = s_Locator.Address + Layout::DeviceLocatorRelOffset;
            s_Slot = s_RelAddress + ReadField<int32_t>(s_RelAddress, 0) + sizeof(int32_t);
            s_WnoOffset = ReadField<uint32_t>(s_Locator.Address + Layout::DeviceLocatorWnoOffset, 0);
        }
    }

    if (s_Slot == 0) {
        Logger::Info("[VRFoveationFix]   device locator : NOT FOUND, nothing further can be read");
        return;
    }

    Logger::Info(
        "[VRFoveationFix]   device locator : pointer at RVA {:#x}, WNO flag at device+{:#x}",
        s_Slot - s_Base, s_WnoOffset
    );

    Logger::Info("[VRFoveationFix] live device:");

    if (!IsReadable(reinterpret_cast<const void*>(s_Slot), sizeof(uintptr_t))) {
        Logger::Info("[VRFoveationFix]   the device pointer slot is not readable");
        return;
    }

    const auto s_Device = ReadField<uintptr_t>(s_Slot, 0);

    if (s_Device == 0) {
        Logger::Info("[VRFoveationFix]   the device pointer is null - start VR and load a mission, then try again");
        return;
    }

    if (!IsReadable(reinterpret_cast<const void*>(s_Device), Layout::DeviceDiagnosticSize)) {
        Logger::Info("[VRFoveationFix]   the device at {:#x} is not readable", s_Device);
        return;
    }

    const auto s_FovTangents = ReadField<std::array<float, 4>>(s_Device, Layout::DeviceFovTangents);

    const bool s_Plausible = ReadField<uint8_t>(s_Device, Layout::DeviceActive) <= 1 &&
            Plausible(s_FovTangents, Layout::MinFovTangent, Layout::MaxFovTangent);

    Logger::Info("[VRFoveationFix]   {:<24} : {:#x}", "device object", s_Device);
    Logger::Info(
        "[VRFoveationFix]   {:<24} : RVA {:#x}   <- this identifies the backend class", "device vtable",
        ReadField<uintptr_t>(s_Device, 0) - s_Base
    );
    Logger::Info("[VRFoveationFix]   {:<24} : {}", "looks like a VR device", s_Plausible ? "yes" : "no");

    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "mode          +0x220",
        ReadField<uint32_t>(s_Device, Layout::DeviceMode)
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "cached mode   +0x30C",
        ReadField<uint32_t>(s_Device, Layout::DeviceCachedMode)
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "active        +0x319",
        ReadField<uint8_t>(s_Device, Layout::DeviceActive)
    );

    if (s_WnoOffset != 0 && IsReadable(reinterpret_cast<const void*>(s_Device + s_WnoOffset), 1)) {
        Logger::Info(
            "[VRFoveationFix]   {:<24} : {}", fmt::format("foveation     +{:#x}", s_WnoOffset),
            ReadField<uint8_t>(s_Device, s_WnoOffset)
        );
    }

    Logger::Info("[VRFoveationFix]   {:<24} : {}", "fov tangents  +0x420", FormatFloats(s_FovTangents));
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "scale ratios  +0x490",
        FormatFloats(ReadField<std::array<float, 4>>(s_Device, Layout::DeviceScaleRatios))
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "overlay mask  +0x4C0",
        FormatFloats(ReadField<std::array<float, 2>>(s_Device, Layout::DeviceOverlayMask))
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "transition    +0x4D8",
        ReadField<uint32_t>(s_Device, Layout::DeviceTransition)
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {} x {}", "eye size      +0x510",
        ReadField<uint32_t>(s_Device, Layout::DeviceWidth), ReadField<uint32_t>(s_Device, Layout::DeviceHeight)
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "layers        +0x520",
        ReadField<uint16_t>(s_Device, Layout::DeviceLayerCount)
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "texture       +0x530",
        fmt::ptr(ReadField<void*>(s_Device, Layout::DeviceTexture))
    );
    Logger::Info(
        "[VRFoveationFix]   {:<24} : {}", "view          +0x538",
        fmt::ptr(ReadField<void*>(s_Device, Layout::DeviceView))
    );

    std::lock_guard s_Lock(m_DeviceLock);

    if (m_DeviceFieldsSaved) {
        Logger::Info(
            "[VRFoveationFix]   stock values saved from device {:#x}: scale ratios {}, overlay mask {}", m_Device,
            FormatFloats(std::array<float, 4>{
                m_StockScaleRatios[0], m_StockScaleRatios[1], m_StockScaleRatios[2], m_StockScaleRatios[3]
            }),
            FormatFloats(std::array<float, 2>{m_StockOverlayMask[0], m_StockOverlayMask[1]})
        );
    }

    Logger::Info("[VRFoveationFix] --- end of diagnostics ---");
}

DEFINE_ZHM_PLUGIN(VRFoveationFix)
