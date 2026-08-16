#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VRFov {
    /**
     * Result of a signature scan over the game's code section.
     * `HitCount` saturates at 2 - the callers only care whether a match is unique.
     */
    struct ScanResult {
        uintptr_t Address = 0;
        size_t HitCount = 0;

        bool IsUnique() const { return HitCount == 1 && Address != 0; }
    };

    /** Parses a "48 8B 0D ?? ?? ?? ??" style pattern into bytes plus an SDK style "xxx????" mask. */
    bool ParsePattern(const char* p_Pattern, std::vector<uint8_t>& p_OutBytes, std::string& p_OutMask);

    /**
     * Scans the game executable's code section for the given pattern.
     *
     * The SDK's own scanner (Util::ProcessUtils::SearchPattern) is not exported to mods, and
     * SDK()->PatchCode stops at the first hit. Refusing to patch an ambiguous signature is the
     * safety property this mod cares about most, so it scans itself and reports the hit count.
     */
    ScanResult ScanCode(const char* p_Pattern);

    /** Returns true if the given range is committed and readable, without faulting. */
    bool IsReadable(const void* p_Address, size_t p_Size);

    /** A reversible byte patch. Captures the original bytes when applied. */
    class MemoryPatch {
    public:
        MemoryPatch() = default;
        MemoryPatch(const char* p_Name, uintptr_t p_Address, const uint8_t* p_Fix, size_t p_FixSize);

        /** Writes the fix, capturing the original bytes first. Verifies the write stuck. */
        bool Apply();

        /** Writes the captured original bytes back. */
        bool Revert();

        bool IsApplied() const { return m_Applied; }
        const char* Name() const { return m_Name; }
        uintptr_t Address() const { return m_Address; }

    private:
        bool Write(const std::vector<uint8_t>& p_Bytes) const;

        const char* m_Name = "";
        uintptr_t m_Address = 0;
        std::vector<uint8_t> m_Original;
        std::vector<uint8_t> m_Fix;
        bool m_Applied = false;
    };
}
