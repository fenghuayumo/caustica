#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"

#include <engine/App.h>
#include <backend/GpuDevice.h>
#include <caustica/version.h>
#include <imgui.h>

namespace caustica::editor
{

void EditorUI::BuildAboutPanel()
{
    if (!m_editorUI.ShowAbout)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.f * m_currentScale, 320.f * m_currentScale), ImGuiCond_Appearing);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (!ImGui::Begin("About Caustica", &m_editorUI.ShowAbout, flags))
    {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Caustica");
    ImGui::TextDisabled("GPU-accelerated physically based rendering");
    ImGui::Separator();

    ImGui::TextWrapped(
        "Caustica is an interactive renderer for physically based path tracing, "
        "real-time scene editing, and Gaussian splat visualization.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Use the editor to inspect scenes, tune rendering features, and preview "
        "high-fidelity lighting and materials on supported GPUs.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Version: %s", caustica::kVersionString);
    if (m_sceneEditor.app())
    {
        if (auto* device = m_sceneEditor.app()->getGpuDevice())
            ImGui::Text("Renderer: %s", device->getRendererString());
    }

    ImGui::Spacing();
    if (ImGui::Button("Close"))
        m_editorUI.ShowAbout = false;

    ImGui::End();
}

} // namespace caustica::editor
