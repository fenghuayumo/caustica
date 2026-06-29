#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorApplication.h"
#include "common/ImGuiManager.h"

#include <render/Core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneGraph.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/Passes/Debug/Korgi.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <game/GameScene.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

#include <inttypes.h>
#include <backend/GpuDevice.h>
void EditorUI::BuildSystemPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("System")) //, ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
            if (ImGui::Button("Reload Shaders (requires VS .hlsl->.bin build)"))
                m_ui.Invalidation.ShaderReloadRequested = true;

            ImGui::Checkbox("Render when out of focus", &m_ui.RenderWhenOutOfFocus);
            if (ImGui::IsItemHovered()) 
                ImGui::SetTooltip("Render loop will pause when app window is out of focus. Note: Reference mode will accumulate until all frames are done.");
        
        
            {
                //RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););

                if (ImGui::CollapsingHeader("Capture scripts and tools"))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                    m_sceneEditor.GetCaptureScriptManager()->ScriptMainUI(warnColor, categoryColor, layout.indent, m_currentScale);
                }

#if CAUSTICA_WITH_PYTHON
                if (ImGui::CollapsingHeader("Python scripting"))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                    BuildPythonScriptingUI(layout.indent);
                }
#endif
            }

            if (ImGui::CollapsingHeader("Info")) //, ImGuiTreeNodeFlags_DefaultOpen))
            {
                caustica::VideoMemoryInfo videoMemoryInfo;
                if (caustica::GpuDevice* gpuDevice = GetGpuDevice();
                    gpuDevice && gpuDevice->QueryVideoMemoryInfo(videoMemoryInfo))
                {
                    ImGui::TextColored(categoryColor, "Video memory:");
                    const uint64_t budget = videoMemoryInfo.budget / (1024 * 1024);
                    const uint64_t currentUsage = videoMemoryInfo.currentUsage / (1024 * 1024);
                    const uint64_t availableForReservation = videoMemoryInfo.availableForReservation / (1024 * 1024);
                    const uint64_t currentReservation = videoMemoryInfo.currentReservation / (1024 * 1024);
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););
                    ImGui::Text("Budget:             %7" PRIu64 "MB", budget);
                    ImGui::Text("CurrentUsage:       %7" PRIu64 "MB", currentUsage);
                    ImGui::Text("AvailableForRes.:   %7" PRIu64 "MB", availableForReservation);
                    ImGui::Text("CurrentReservation: %7" PRIu64 "MB", currentReservation);
                }
            }
        }


}

void EditorUI::BuildCameraPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Camera", 0/*ImGuiTreeNodeFlags_DefaultOpen*/))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
            std::vector<std::string> options; options.push_back("Free flight");
            for (uint i = 0; i < m_sceneEditor.GetSceneCameraCount(); i++)
                options.push_back("Scene cam " + std::to_string(i));
            uint& currentlySelected = m_sceneEditor.SelectedCameraIndex();
            currentlySelected = std::min(currentlySelected, (uint)m_sceneEditor.GetSceneCameraCount() - 1);
            if (ImGui::BeginCombo("Motion", options[currentlySelected].c_str()))
            {
                for (uint i = 0; i < m_sceneEditor.GetSceneCameraCount(); i++)
                {
                    bool is_selected = i == currentlySelected;
                    if (ImGui::Selectable(options[i].c_str(), is_selected))
                        currentlySelected = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (currentlySelected == 0)
            {
                ImGui::Text("Camera position: "); 
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                if (ImGui::Button("Save to file", ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetTextLineHeightWithSpacing()))) m_sceneEditor.SaveCurrentCamera(); ImGui::SameLine();
                if (ImGui::Button("Load from file", ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetTextLineHeightWithSpacing()))) m_sceneEditor.LoadCurrentCamera();
                if (ImGui::Button("Save to clipboard", ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetTextLineHeightWithSpacing()))) ImGui::SetClipboardText(m_sceneEditor.GetCurrentCameraPosDirUp().c_str()); ImGui::SameLine();
                const char *cpbrdtxt = ImGui::GetClipboardText();
                if (ImGui::Button("Load from clipboard", ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetTextLineHeightWithSpacing()))) m_sceneEditor.SetCurrentCameraPosDirUp(cpbrdtxt?cpbrdtxt:"");
            }

    #if 1
            RESET_ON_CHANGE( ImGui::InputFloat("Aperture", &m_ui.CameraAperture, 0.001f, 0.01f, "%.4f") );
            m_ui.CameraAperture = dm::clamp(m_ui.CameraAperture, 0.0f, 1.0f);

            RESET_ON_CHANGE( ImGui::InputFloat("Focal Distance", &m_ui.CameraFocalDistance, 0.1f) );
            m_ui.CameraFocalDistance = dm::clamp(m_ui.CameraFocalDistance, 0.001f, 1e16f);
            ImGui::SliderFloat("Keyboard move speed", &m_ui.CameraMoveSpeed, 0.1f, 10.0f);

            float cameraFOV = 2.0f * dm::degrees(m_sceneEditor.GetCameraVerticalFOV());
            if (ImGui::InputFloat("Vertical FOV", &cameraFOV, 0.1f))
            {
                cameraFOV = dm::clamp(cameraFOV, 1.0f, 360.0f);
                m_ui.ResetAccumulation = true;
                m_sceneEditor.SetCameraVerticalFOV(dm::radians(cameraFOV / 2.0f));
            }

            RESET_ON_CHANGE( ImGui::InputFloat("CameraAntiRRSleepJitter", &m_ui.CameraAntiRRSleepJitter, 0.001f ) );
            m_ui.CameraAntiRRSleepJitter = clamp( m_ui.CameraAntiRRSleepJitter, 0.0f, 1.0f );
    #endif
        }


}

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"

void EditorUI::BuildPythonScriptingUI(float indent)
{
    auto& scripting = m_sceneEditor.GetPythonScripting();
    if (!scripting)
    {
        ImGui::TextDisabled("Python scripting host unavailable.");
        return;
    }

    if (!scripting->IsInitialized())
    {
        if (ImGui::Button("Initialize Python interpreter"))
            scripting->Initialize();
        ImGui::TextDisabled("(Click to start the embedded CPython runtime.)");
        return;
    }

    // ---- File-based scripts ---------------------------------------------
    ImGui::TextUnformatted("Run Python script (.py):");
    static char pathBuffer[1024] = {};
    if (m_pythonScriptPath.size() && pathBuffer[0] == '\0')
    {
        std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", m_pythonScriptPath.c_str());
    }
    ImGui::PushItemWidth(-200.0f * m_currentScale);
    ImGui::InputText("##PythonScriptPath", pathBuffer, sizeof(pathBuffer));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##PyScript"))
    {
        std::string picked;
        if (caustica::FileDialog(true, "Python Scripts (*.py)\0*.py\0All\0*.*\0", picked))
        {
            std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", picked.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Run##PyScript"))
    {
        m_pythonScriptPath = pathBuffer;
        if (!m_pythonScriptPath.empty())
            scripting->QueueScriptFile(std::filesystem::path(m_pythonScriptPath));
    }

    ImGui::Separator();

    // ---- Inline expression / snippet ------------------------------------
    ImGui::TextUnformatted("Inline expression:");
    static char inlineBuffer[8192] = "import caustica\nfor mat in caustica.app().scene.get_materials():\n    print(mat.name, mat.base_color)\n";
    ImGui::InputTextMultiline("##PythonInline", inlineBuffer, sizeof(inlineBuffer),
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 6.0f));
    if (ImGui::Button("Run inline"))
    {
        m_pythonInlineCode = inlineBuffer;
        scripting->QueueScriptString(m_pythonInlineCode, "<UI inline>");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear inline"))
        inlineBuffer[0] = '\0';

    // ---- Output log ------------------------------------------------------
    std::string newLog = scripting->ConsumeOutputLog();
    if (!newLog.empty())
        m_pythonOutputLog += newLog;

    ImGui::Separator();
    ImGui::TextUnformatted("Captured stdout/stderr:");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear log"))
        m_pythonOutputLog.clear();
    ImGui::BeginChild("##PythonOutput",
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 8.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(m_pythonOutputLog.c_str());
    if (!newLog.empty())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
#endif // CAUSTICA_WITH_PYTHON


} // namespace caustica::editor

