#include "ProcessInfo.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>

namespace VRFov {
    namespace {
        const IMAGE_NT_HEADERS* GetNtHeaders() {
            const auto s_Module = GetModuleHandleW(nullptr);

            if (!s_Module) {
                return nullptr;
            }

            const auto* s_DosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(s_Module);
            const auto* s_NtHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(
                reinterpret_cast<uintptr_t>(s_Module) + s_DosHeader->e_lfanew
            );

            return s_NtHeader->Signature == IMAGE_NT_SIGNATURE ? s_NtHeader : nullptr;
        }

        /** Substrings of a module name that mean it is part of a VR runtime. */
        constexpr const char* c_VrModuleNames[] = {
            "libovr", "oculus", "openvr", "vrclient", "openxr", "vdxr", "steamvr",
        };
    }

    uintptr_t GetModuleBase() {
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }

    uint32_t GetImageTimestamp() {
        const auto* s_NtHeader = GetNtHeaders();
        return s_NtHeader ? s_NtHeader->FileHeader.TimeDateStamp : 0;
    }

    std::vector<std::string> GetLoadedVrModules() {
        std::vector<std::string> s_Modules;

        const auto s_Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);

        if (s_Snapshot == INVALID_HANDLE_VALUE) {
            return s_Modules;
        }

        MODULEENTRY32W s_Entry;
        s_Entry.dwSize = sizeof(s_Entry);

        for (auto s_More = Module32FirstW(s_Snapshot, &s_Entry); s_More;
             s_More = Module32NextW(s_Snapshot, &s_Entry)) {
            // Module file names are ASCII in practice, so a plain narrowing is enough.
            std::string s_Name;

            for (const wchar_t* s_Char = s_Entry.szModule; *s_Char != L'\0'; ++s_Char) {
                s_Name.push_back(*s_Char < 0x80 ? static_cast<char>(*s_Char) : '?');
            }

            std::string s_Lower = s_Name;

            std::transform(
                s_Lower.begin(), s_Lower.end(), s_Lower.begin(),
                [](unsigned char p_Char) { return static_cast<char>(std::tolower(p_Char)); }
            );

            for (const auto* s_Needle : c_VrModuleNames) {
                if (s_Lower.find(s_Needle) == std::string::npos) {
                    continue;
                }

                if (std::find(s_Modules.begin(), s_Modules.end(), s_Name) == s_Modules.end()) {
                    s_Modules.push_back(s_Name);
                }

                break;
            }
        }

        CloseHandle(s_Snapshot);

        return s_Modules;
    }
}
