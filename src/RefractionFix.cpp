#include "RefractionFix.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <cstring>

#include <MinHook.h>

#include <Logging.h>

#include "CodePatcher.h"
#include "Layout.h"

namespace VRFov {
    namespace {
        /**
         * The pass that draws refractive and transparent geometry. Ten arguments, all integer or
         * pointer sized, taken from the one call site the game has for it. A detour has to pass all
         * of them through untouched, so the count matters; the types do not, beyond their size.
         */
        using ScopeFunc_t = void* (__fastcall*)(
            void*, void*, void*, void*, void*, void*, void*, void*, void*, void*
        );

        /** The refraction depth copy. Six arguments, the last three of them 32 bit. */
        using CopyFunc_t = void* (__fastcall*)(void*, void*, void*, uint32_t, uint32_t, uint32_t);

        ScopeFunc_t g_ScopeOriginal = nullptr;
        CopyFunc_t g_CopyOriginal = nullptr;

        std::atomic<uint64_t> g_Swaps = 0;
        std::atomic<uint64_t> g_PassThroughs = 0;
        std::atomic<uint64_t> g_Anomalies = 0;

        /**
         * The render context the refractive and transparent pass is running on, for this thread
         * only. Null everywhere else, which is what keeps the swap below out of every other pass.
         */
        thread_local void* t_ScopeContext = nullptr;

        void Count(std::atomic<uint64_t>& p_Counter) { p_Counter.fetch_add(1, std::memory_order_relaxed); }

        /** Puts the scope back on the way out, including when the pass unwinds instead of returning. */
        struct ScopeGuard {
            void* Previous;

            explicit ScopeGuard(void* p_Context) : Previous(t_ScopeContext) { t_ScopeContext = p_Context; }
            ~ScopeGuard() { t_ScopeContext = Previous; }
        };

        /** Puts the view count back the same way. Only ever restores what this fix wrote. */
        struct CountGuard {
            uint32_t* Slot = nullptr;

            ~CountGuard() {
                if (Slot == nullptr) {
                    return;
                }

                // Anything else means something underneath moved the count on its own, and
                // writing four over that would be a guess.
                if (*Slot == Layout::ViewCountPhysical) {
                    *Slot = Layout::ViewCountLogical;
                }
                else {
                    Count(g_Anomalies);
                }
            }
        };

        void* __fastcall Detour_RefractionScope(
            void* p_Context, void* p_Arg2, void* p_Arg3, void* p_Arg4, void* p_Arg5,
            void* p_Arg6, void* p_Arg7, void* p_Arg8, void* p_Arg9, void* p_Arg10
        ) {
            // Restored rather than cleared, so a nested call cannot leave the scope early.
            const ScopeGuard s_Scope(p_Context);

            return g_ScopeOriginal(
                p_Context, p_Arg2, p_Arg3, p_Arg4, p_Arg5, p_Arg6, p_Arg7, p_Arg8, p_Arg9, p_Arg10
            );
        }

        void* __fastcall Detour_RefractionDepthCopy(
            void* p_Arg1, void* p_Arg2, void* p_Arg3, uint32_t p_Arg4, uint32_t p_Arg5, uint32_t p_Arg6
        ) {
            auto* s_Context = static_cast<uint32_t*>(t_ScopeContext);

            CountGuard s_Guard;

            if (s_Context != nullptr) {
                const auto s_Top = s_Context[Layout::ContextCountTop / sizeof(uint32_t)];

                // Out of range means this is not the stack this fix knows about. Leave it alone.
                if (s_Top <= Layout::MaxContextCountTop) {
                    auto* s_Current = s_Context + s_Top;

                    if (*s_Current == Layout::ViewCountLogical) {
                        *s_Current = Layout::ViewCountPhysical;
                        s_Guard.Slot = s_Current;
                    }
                }
            }

            Count(s_Guard.Slot != nullptr ? g_Swaps : g_PassThroughs);

            return g_CopyOriginal(p_Arg1, p_Arg2, p_Arg3, p_Arg4, p_Arg5, p_Arg6);
        }

        /** Resolves the call in a located site to the address it calls. Zero if not unique. */
        uintptr_t ResolveCallTarget(const Layout::CallSite& p_Site) {
            const auto s_Scan = ScanCode(p_Site.Pattern);

            if (!s_Scan.IsUnique()) {
                Logger::Error(
                    "[VRFoveationFix] The call to {} could not be located uniquely in this build ({} matches).",
                    p_Site.Name, s_Scan.HitCount
                );
                return 0;
            }

            const auto s_Call = s_Scan.Address + p_Site.CallOffset;

            if (*reinterpret_cast<const uint8_t*>(s_Call) != 0xE8) {
                Logger::Error("[VRFoveationFix] The call to {} is not where it should be.", p_Site.Name);
                return 0;
            }

            int32_t s_Displacement;
            memcpy(&s_Displacement, reinterpret_cast<const void*>(s_Call + 1), sizeof(s_Displacement));

            // Relative to the end of the five byte instruction.
            const auto s_Target = s_Call + 5 + s_Displacement;

            if (!IsReadable(reinterpret_cast<const void*>(s_Target), 1)) {
                Logger::Error("[VRFoveationFix] The call to {} does not lead anywhere readable.", p_Site.Name);
                return 0;
            }

            return s_Target;
        }
    }

    RefractionFix::~RefractionFix() {
        Uninstall();
    }

    uint64_t RefractionFix::SwapCount() { return g_Swaps.load(std::memory_order_relaxed); }
    uint64_t RefractionFix::PassThroughCount() { return g_PassThroughs.load(std::memory_order_relaxed); }
    uint64_t RefractionFix::AnomalyCount() { return g_Anomalies.load(std::memory_order_relaxed); }

    bool RefractionFix::Install() {
        if (m_Installed) {
            return true;
        }

        const auto s_Scope = ResolveCallTarget(Layout::RefractionScopeCall);

        if (s_Scope == 0) {
            return false;
        }

        uintptr_t s_Copy = 0;

        for (const auto& s_Site : Layout::RefractionCopyCalls) {
            const auto s_Target = ResolveCallTarget(s_Site);

            if (s_Target == 0) {
                return false;
            }

            // Both calls are the same function in the same pass. If a build ever disagrees, this
            // is not the code this fix was written against.
            if (s_Copy != 0 && s_Target != s_Copy) {
                Logger::Error("[VRFoveationFix] The two refraction depth copies do not call the same function.");
                return false;
            }

            s_Copy = s_Target;
        }

        if (s_Copy == s_Scope) {
            Logger::Error("[VRFoveationFix] The refraction depth copy and its pass resolved to one function.");
            return false;
        }

        const auto s_Status = MH_Initialize();

        if (s_Status != MH_OK && s_Status != MH_ERROR_ALREADY_INITIALIZED) {
            Logger::Error("[VRFoveationFix] The hook library could not start ({}).", MH_StatusToString(s_Status));
            return false;
        }

        const auto s_Create = [](uintptr_t p_Target, void* p_Detour, void** p_Original, const char* p_What) {
            const auto s_Result = MH_CreateHook(reinterpret_cast<void*>(p_Target), p_Detour, p_Original);

            if (s_Result != MH_OK) {
                Logger::Error(
                    "[VRFoveationFix] {} could not be hooked ({}).", p_What, MH_StatusToString(s_Result)
                );
                return false;
            }

            return true;
        };

        if (!s_Create(
            s_Copy, reinterpret_cast<void*>(&Detour_RefractionDepthCopy),
            reinterpret_cast<void**>(&g_CopyOriginal), "The refraction depth copy"
        )) {
            return false;
        }

        if (!s_Create(
            s_Scope, reinterpret_cast<void*>(&Detour_RefractionScope),
            reinterpret_cast<void**>(&g_ScopeOriginal), "The refractive and transparent pass"
        )) {
            MH_RemoveHook(reinterpret_cast<void*>(s_Copy));
            return false;
        }

        // The copy first, so it is already running - as a pass through, with no scope set yet -
        // before the scope that gates it goes live. Removal below reverses this.
        if (MH_EnableHook(reinterpret_cast<void*>(s_Copy)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(s_Scope)) != MH_OK) {
            Logger::Error("[VRFoveationFix] The refraction detours could not be enabled.");
            MH_RemoveHook(reinterpret_cast<void*>(s_Scope));
            MH_RemoveHook(reinterpret_cast<void*>(s_Copy));
            return false;
        }

        m_ScopeFunction = s_Scope;
        m_CopyFunction = s_Copy;
        m_Installed = true;

        Logger::Debug(
            "[VRFoveationFix] Refraction depth copy at {}, its pass at {}.",
            fmt::ptr(reinterpret_cast<void*>(s_Copy)), fmt::ptr(reinterpret_cast<void*>(s_Scope))
        );

        return true;
    }

    void RefractionFix::Uninstall() {
        if (!m_Installed) {
            return;
        }

        // The scope first: with nothing setting it, the copy detour is a pass through by the time
        // it is taken out. MinHook suspends the other threads for both.
        MH_RemoveHook(reinterpret_cast<void*>(m_ScopeFunction));
        MH_RemoveHook(reinterpret_cast<void*>(m_CopyFunction));

        // This mod's own copy of the library, so this only ever affects the two hooks above. It
        // gives the trampoline pages back, which matters when the mod is unloaded and reloaded.
        MH_Uninitialize();

        m_Installed = false;
        m_ScopeFunction = 0;
        m_CopyFunction = 0;

        g_ScopeOriginal = nullptr;
        g_CopyOriginal = nullptr;
    }
}
