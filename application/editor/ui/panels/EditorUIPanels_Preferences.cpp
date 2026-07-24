#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"

#include <engine/RenderSessionApi.h>
#include <backend/GpuDevice.h>
#include <imgui.h>

namespace caustica::editor
{

void EditorUI::BuildPreferencesPanel(const PanelLayout& layout)
{
    if (!m_editorUI.ShowPreferences)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.f * m_currentScale, 520.f * m_currentScale), ImGuiCond_Appearing);

    if (!ImGui::Begin("Preferences", &m_editorUI.ShowPreferences))
    {
        ImGui::End();
        return;
    }

    RAII_SCOPE(ImGui::PushItemWidth(layout.defItemWidth);, ImGui::PopItemWidth(););

    if (m_sceneEditor.app())
    {
        ImGui::TextUnformatted(getGpuDevice()->getRendererString());
        ImGui::TextUnformatted(caustica::resolutionInfo(*m_sceneEditor.app()).c_str());
        ImGui::TextUnformatted(caustica::fpsInfo(*m_sceneEditor.app()).c_str());
        ImGui::Separator();
    }

    BuildDisplayPerformancePanel(layout);
    BuildSystemPanel(layout);

    ImGui::End();
}

} // namespace caustica::editor
