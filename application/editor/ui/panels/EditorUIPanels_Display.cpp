#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <game/GameScene.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

bool EditorUI::BuildUIScriptsAndEtc(void)
{
    bool scriptsActive = false;
    if (m_sceneEditor.GetCaptureScriptManager()->ScriptProgressUI())
        scriptsActive = true;

    if (scriptsActive)
        ImGui::Text("=================================================");

    return scriptsActive;
}

void EditorUI::BuildUIResolutionPicker()
{
    if (!ImGui::BeginPopupModal("Resolution Picker", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    struct Resolution { int w, h; const char* label; };
    static const Resolution standardResolutions[] = {
        { 1280,  720, "1280 x 720   (16:9)" },
        { 1280,  800, "1280 x 800   (16:10)" },
        { 1366,  768, "1366 x 768   (16:9)" },
        { 1440,  900, "1440 x 900   (16:10)" },
        { 1600,  900, "1600 x 900   (16:9)" },
        { 1680, 1050, "1680 x 1050  (16:10)" },
        { 1920, 1080, "1920 x 1080  (16:9)" },
        { 1920, 1200, "1920 x 1200  (16:10)" },
        { 2560, 1440, "2560 x 1440  (16:9)" },
        { 2560, 1600, "2560 x 1600  (16:10)" },
        { 3840, 2160, "3840 x 2160  (16:9)" },
        { 3840, 2400, "3840 x 2400  (16:10)" },
    };

    const GLFWvidmode* monitorMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    int currentW = (int)m_sceneEditor.GetDisplaySize().x;
    int currentH = (int)m_sceneEditor.GetDisplaySize().y;

    ImGui::Text("Click to change resolution:");
    ImGui::Separator();

    for (const auto& res : standardResolutions)
    {
        if (res.w > monitorMode->width || res.h > monitorMode->height)
            continue;

        bool isCurrent = (res.w == currentW && res.h == currentH);
        std::string displayLabel = std::string(res.label) + (isCurrent ? "  [current]" : "");
        if (ImGui::Selectable(displayLabel.c_str(), isCurrent))
        {
            glfwSetWindowSize(GetGpuDevice()->GetWindow(), res.w, res.h);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Cancel", { -1, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void EditorUI::BuildUIPerformancePresets()
{
    if (!ImGui::BeginPopupModal("Performance Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Click to select a performance preset:");
    ImGui::Separator();

    for (const auto& preset : s_performancePresets)
    {
        bool isCurrent = MatchesPreset(m_ui, preset);
        std::string displayLabel = std::string(preset.Name) + (isCurrent ? "  [current]" : "");
        if (ImGui::Selectable(displayLabel.c_str(), isCurrent))
        {
            ApplyPreset(m_ui, preset);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Cancel", { -1, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void EditorUI::DLSSFGSelectorUI()
{
#if !CAUSTICA_WITH_STREAMLINE
    return;
#else
    const char* items[] = { "Off", "2x", "3x", "4x" };
    const int itemCount = IM_ARRAYSIZE(items);

    static int currentItem = 0;
    if (ImGui::BeginCombo("Frame Generation", items[currentItem]))
    {
        for (int itemId = 0; itemId < itemCount; itemId++)
        {
            UI_SCOPED_DISABLE(itemId > m_settings.DLSSFGMaxNumFramesToGenerate);

            bool isSelected = (currentItem == itemId);
            if (ImGui::Selectable(items[itemId], isSelected))
                currentItem = itemId;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    m_settings.DLSSFGMode = (currentItem > 0)
        ? caustica::StreamlineInterface::DLSSGMode::eOn
        : caustica::StreamlineInterface::DLSSGMode::eOff;

    m_settings.DLSSFGNumFramesToGenerate = (m_settings.DLSSFGMode == caustica::StreamlineInterface::DLSSGMode::eOn) ? currentItem : 1;

    if (!m_settings.RealtimeMode)
        ImGui::TextColored(warnColor, "Note: DLSS-G is DISABLED in Reference PT mode");
#endif
};



void EditorUI::BuildDisplayPerformancePanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Display and performance")) //, ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
            
            {
                if (ImGui::Button(StringFormat("Resolution:  %dx%d (click to change)", m_sceneEditor.GetDisplaySize().x, m_sceneEditor.GetDisplaySize().y, m_sceneEditor.GetRenderSize().x, m_sceneEditor.GetRenderSize().y).c_str(), { -1, 0 }))
                    ImGui::OpenPopup("Resolution Picker");
                BuildUIResolutionPicker();
            }

            {
                const char* currentPresetName = "Custom";
                for (const auto& preset : s_performancePresets)
                    if (MatchesPreset(m_ui, preset)) { currentPresetName = preset.Name; break; }
                if (ImGui::Button(StringFormat("Perf. preset: %s (click to change)", currentPresetName).c_str(), { -1, 0 }))
                    ImGui::OpenPopup("Performance Preset");
                BuildUIPerformancePresets();
            }

            {
                RAII_SCOPE(ImGui::PushID("DispAndPerf"); , ImGui::PopID(); );
                DLSSFGSelectorUI();
            }

            {
#if CAUSTICA_WITH_STREAMLINE
                UI_SCOPED_DISABLE(m_settings.ActualDLSSFGMode() != SI::DLSSGMode::eOff);
#endif
                ImGui::Checkbox("VSync", &m_settings.EnableVsync);
                bool fpsLimiter = m_settings.FPSLimiter != 0;
                ImGui::SameLine();
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();
                ImGui::Text("Cap fps to ");
                ImGui::SameLine();
                std::array<int, 8> fpsOptions{ 0, /*1,*/ 2, 5, 10, 15, 30, 60, 120 }; auto curr = std::find(fpsOptions.begin(), fpsOptions.end(), m_settings.FPSLimiter);
                int fpsLimitIndex = (curr != fpsOptions.end()) ? (int(curr - fpsOptions.begin())) : (0);
                if (ImGui::Combo("##FPSLIMITER", &fpsLimitIndex, "disabled\0" /* " 1 \0" */ " 2 \0 5 \0 10 \0 15 \0 30 \0 60 \0 120 \0\0"))
                    m_settings.FPSLimiter = fpsOptions[dm::clamp(fpsLimitIndex, 0, (int)fpsOptions.size() - 1)];
            }

        }


}


} // namespace caustica::editor

