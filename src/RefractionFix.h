#pragma once

#include <cstdint>

namespace VRFov {
    /**
     * Makes the refraction depth copy run on the two physical eye views while everything around it
     * keeps the four logical views the geometry needs.
     *
     * Two detours. The outer one wraps the refractive and transparent pass and does nothing but
     * remember which render context that pass is running on, for the calling thread only. The inner
     * one wraps the refraction depth copy and, when it is running inside that scope and the context
     * really is on a count of four, swaps the count to two for the duration of the call.
     *
     * Ported from the wrappers upstream splices into the two call sites; in process, detours give
     * the same scoping and clean removal without hand written code. See Layout.h for the sites and
     * PLAN.md for why this is needed at all.
     */
    class RefractionFix {
    public:
        ~RefractionFix();

        /**
         * Locates both functions and installs the detours. Changes nothing and returns false if a
         * signature is not unique, if the two copy calls disagree about their target, or if the
         * hooks cannot be created.
         */
        bool Install();

        /** Removes the detours. Safe to call when nothing was installed. */
        void Uninstall();

        bool IsInstalled() const { return m_Installed; }

        /** Address of the pass whose scope the fix is limited to. Zero until located. */
        uintptr_t ScopeFunction() const { return m_ScopeFunction; }

        /** Address of the refraction depth copy. Zero until located. */
        uintptr_t CopyFunction() const { return m_CopyFunction; }

        /** Times the copy ran with the count swapped. Climbs whenever VR is rendering. */
        static uint64_t SwapCount();

        /** Times the copy ran unchanged, because it was out of scope or the count was not four. */
        static uint64_t PassThroughCount();

        /** Times the count was not what this fix had left behind. Should stay zero. */
        static uint64_t AnomalyCount();

    private:
        bool m_Installed = false;
        uintptr_t m_ScopeFunction = 0;
        uintptr_t m_CopyFunction = 0;
    };
}
