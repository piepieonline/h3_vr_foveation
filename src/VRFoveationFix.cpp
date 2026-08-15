#include "VRFoveationFix.h"

#include <Logging.h>
#include <IconsMaterialDesign.h>
#include <Globals.h>

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZScene.h>

void VRFoveationFix::OnEngineInitialized() {
    Logger::Info("VRFoveationFix has been initialized!");

    // Register a function to be called on every game frame while the game is in play mode.
    const ZMemberDelegate<VRFoveationFix, void(const SGameUpdateEvent&)> s_Delegate(this, &VRFoveationFix::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    // Install a hook to print the name of the scene every time the game loads a new one.
    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &VRFoveationFix::OnLoadScene);
}

VRFoveationFix::~VRFoveationFix() {
    // Unregister our frame update function when the mod unloads.
    const ZMemberDelegate<VRFoveationFix, void(const SGameUpdateEvent&)> s_Delegate(this, &VRFoveationFix::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

void VRFoveationFix::OnDrawMenu() {
    // Toggle our message when the user presses our button.
    if (ImGui::Button(ICON_MD_LOCAL_FIRE_DEPARTMENT " VRFoveationFix")) {
        m_ShowMessage = !m_ShowMessage;
    }
}

void VRFoveationFix::OnDrawUI(bool p_HasFocus) {
    if (m_ShowMessage) {
        // Show a window for our mod.
        if (ImGui::Begin("VRFoveationFix", &m_ShowMessage)) {
            // Only show these when the window is expanded.
            ImGui::Text("Hello from VRFoveationFix!");
        }
        ImGui::End();
    }
}

void VRFoveationFix::OnFrameUpdate(const SGameUpdateEvent &p_UpdateEvent) {
    // This function is called every frame while the game is in play mode.
}

DEFINE_PLUGIN_DETOUR(VRFoveationFix, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_Parameters) {
    Logger::Debug("Loading scene: {}", p_Parameters.m_SceneResource);
    return { HookAction::Continue() };
}

DECLARE_ZHM_PLUGIN(VRFoveationFix);
