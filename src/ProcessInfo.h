#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VRFov {
    /** Base address of the game executable, so addresses can be reported as RVAs. */
    uintptr_t GetModuleBase();

    /** PE TimeDateStamp of the game executable. Identifies the build the offsets were taken from. */
    uint32_t GetImageTimestamp();

    /** Names of the loaded modules that look like a VR runtime, which identifies the backend. */
    std::vector<std::string> GetLoadedVrModules();
}
