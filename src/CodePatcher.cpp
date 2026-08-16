#include "CodePatcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstring>

#include <Logging.h>

namespace VRFov {
    namespace {
        struct CodeRange {
            uintptr_t Base = 0;
            size_t Size = 0;
        };

        /** The game executable's code section, the same range ModSDK::PatchCode scans. */
        const CodeRange& GetCodeRange() {
            static CodeRange s_Range = [] {
                CodeRange s_Result;

                const auto s_Module = GetModuleHandleW(nullptr);

                if (!s_Module) {
                    return s_Result;
                }

                const auto* s_DosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(s_Module);
                const auto* s_NtHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(
                    reinterpret_cast<uintptr_t>(s_Module) + s_DosHeader->e_lfanew
                );

                if (s_NtHeader->Signature != IMAGE_NT_SIGNATURE) {
                    return s_Result;
                }

                s_Result.Base = reinterpret_cast<uintptr_t>(s_Module) + s_NtHeader->OptionalHeader.BaseOfCode;
                s_Result.Size = s_NtHeader->OptionalHeader.SizeOfCode;

                return s_Result;
            }();

            return s_Range;
        }

        int HexNibble(char p_Char) {
            if (p_Char >= '0' && p_Char <= '9') {
                return p_Char - '0';
            }

            if (p_Char >= 'a' && p_Char <= 'f') {
                return p_Char - 'a' + 10;
            }

            if (p_Char >= 'A' && p_Char <= 'F') {
                return p_Char - 'A' + 10;
            }

            return -1;
        }
    }

    bool ParsePattern(const char* p_Pattern, std::vector<uint8_t>& p_OutBytes, std::string& p_OutMask) {
        p_OutBytes.clear();
        p_OutMask.clear();

        for (const char* s_Cursor = p_Pattern; *s_Cursor != '\0';) {
            if (*s_Cursor == ' ') {
                ++s_Cursor;
                continue;
            }

            if (s_Cursor[0] == '?') {
                // Accept both "?" and "??".
                p_OutBytes.push_back(0);
                p_OutMask.push_back('?');
                s_Cursor += s_Cursor[1] == '?' ? 2 : 1;
                continue;
            }

            const auto s_High = HexNibble(s_Cursor[0]);
            const auto s_Low = s_Cursor[1] == '\0' ? -1 : HexNibble(s_Cursor[1]);

            if (s_High < 0 || s_Low < 0) {
                return false;
            }

            p_OutBytes.push_back(static_cast<uint8_t>(s_High << 4 | s_Low));
            p_OutMask.push_back('x');
            s_Cursor += 2;
        }

        return !p_OutBytes.empty();
    }

    ScanResult ScanCode(const char* p_Pattern) {
        ScanResult s_Result;

        std::vector<uint8_t> s_Bytes;
        std::string s_Mask;

        if (!ParsePattern(p_Pattern, s_Bytes, s_Mask)) {
            Logger::Error("[VRFoveationFix] Malformed signature: {}", p_Pattern);
            return s_Result;
        }

        const auto& s_Range = GetCodeRange();

        if (s_Range.Base == 0 || s_Range.Size <= s_Bytes.size()) {
            Logger::Error("[VRFoveationFix] Could not determine the game's code section.");
            return s_Result;
        }

        const auto s_SearchEnd = s_Range.Base + s_Range.Size - s_Bytes.size();

        for (auto s_Address = s_Range.Base; s_Address <= s_SearchEnd; ++s_Address) {
            const auto* s_Memory = reinterpret_cast<const uint8_t*>(s_Address);

            bool s_Found = true;

            for (size_t i = 0; i < s_Bytes.size(); ++i) {
                if (s_Mask[i] == '?') {
                    continue;
                }

                if (s_Memory[i] != s_Bytes[i]) {
                    s_Found = false;
                    break;
                }
            }

            if (!s_Found) {
                continue;
            }

            if (s_Result.HitCount == 0) {
                s_Result.Address = s_Address;
            }

            // Two hits is all the caller needs to know that the signature is ambiguous.
            if (++s_Result.HitCount > 1) {
                break;
            }
        }

        return s_Result;
    }

    bool IsReadable(const void* p_Address, size_t p_Size) {
        if (p_Address == nullptr || p_Size == 0) {
            return false;
        }

        auto s_Cursor = reinterpret_cast<uintptr_t>(p_Address);
        const auto s_End = s_Cursor + p_Size;

        while (s_Cursor < s_End) {
            MEMORY_BASIC_INFORMATION s_Info;

            if (VirtualQuery(reinterpret_cast<LPCVOID>(s_Cursor), &s_Info, sizeof(s_Info)) != sizeof(s_Info)) {
                return false;
            }

            if (s_Info.State != MEM_COMMIT) {
                return false;
            }

            constexpr DWORD c_Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

            if ((s_Info.Protect & c_Readable) == 0 || (s_Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }

            s_Cursor = reinterpret_cast<uintptr_t>(s_Info.BaseAddress) + s_Info.RegionSize;
        }

        return true;
    }

    MemoryPatch::MemoryPatch(const char* p_Name, uintptr_t p_Address, const uint8_t* p_Fix, size_t p_FixSize) :
        m_Name(p_Name), m_Address(p_Address), m_Fix(p_Fix, p_Fix + p_FixSize) {}

    bool MemoryPatch::Write(const std::vector<uint8_t>& p_Bytes) const {
        auto* s_Target = reinterpret_cast<void*>(m_Address);

        DWORD s_OldProtect;

        if (!VirtualProtect(s_Target, p_Bytes.size(), PAGE_EXECUTE_READWRITE, &s_OldProtect)) {
            return false;
        }

        memcpy(s_Target, p_Bytes.data(), p_Bytes.size());

        VirtualProtect(s_Target, p_Bytes.size(), s_OldProtect, &s_OldProtect);
        FlushInstructionCache(GetCurrentProcess(), s_Target, p_Bytes.size());

        return true;
    }

    bool MemoryPatch::Apply() {
        if (m_Applied || m_Address == 0 || m_Fix.empty()) {
            return false;
        }

        const auto* s_Target = reinterpret_cast<const uint8_t*>(m_Address);
        m_Original.assign(s_Target, s_Target + m_Fix.size());

        if (!Write(m_Fix)) {
            Logger::Error("[VRFoveationFix] Could not write the patch for '{}'.", m_Name);
            m_Original.clear();
            return false;
        }

        if (memcmp(s_Target, m_Fix.data(), m_Fix.size()) != 0) {
            Logger::Error("[VRFoveationFix] The patch for '{}' did not stick.", m_Name);
            Write(m_Original);
            m_Original.clear();
            return false;
        }

        m_Applied = true;
        return true;
    }

    bool MemoryPatch::Revert() {
        if (!m_Applied) {
            return false;
        }

        if (!Write(m_Original)) {
            Logger::Error("[VRFoveationFix] Could not restore the original code for '{}'.", m_Name);
            return false;
        }

        m_Applied = false;
        return true;
    }
}
