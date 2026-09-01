#include "ui/EditorUIInternal.h"
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>

#include "SceneEditor.h"
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <vector>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>
#include <platform/file_dialog.h>

#if CAUSTICA_WITH_PYTHON
#include "PythonScripting.h"
#endif

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
        RAII_SCOPE(ImGui::PushID("SystemPanel");, ImGui::PopID(););
        if (ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
            if (ImGui::Button("Reload Shaders", ImVec2(-1.f, 0.f)))
                m_runtime.Invalidation.ShaderReloadRequested = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Requires the Visual Studio .hlsl -> .bin build step.");

            SettingsCheckbox("Background Render", &m_editorUI.RenderWhenOutOfFocus);
            if (ImGui::IsItemHovered()) 
                ImGui::SetTooltip("render loop will pause when app window is out of focus. Note: Reference mode will accumulate until all frames are done.");
        
        
            {
                //RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););

                if (ImGui::CollapsingHeader("Capture Scripts & Tools"))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                    m_sceneEditor.captureScriptManager()->ScriptMainUI(warnColor, categoryColor, layout.indent, m_currentScale);
                }

#if CAUSTICA_WITH_PYTHON
                if (ImGui::CollapsingHeader("Python Scripting"))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                    BuildPythonScriptingUI(layout.indent);
                }
#endif
            }

            if (ImGui::CollapsingHeader("Info")) //, ImGuiTreeNodeFlags_DefaultOpen))
            {
                caustica::VideoMemoryInfo videoMemoryInfo;
                if (caustica::GpuDevice* device = getGpuDevice();
                    device && device->queryVideoMemoryInfo(videoMemoryInfo))
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

#if CAUSTICA_WITH_PYTHON

void EditorUI::BuildPythonScriptingUI(float indent)
{
    auto& scripting = m_sceneEditor.pythonScripting();
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
        if (::caustica::FileDialog(true, "Python Scripts (*.py)\0*.py\0All\0*.*\0", picked))
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
    if (ImGui::Button("clear inline"))
        inlineBuffer[0] = '\0';

    // ---- Output log ------------------------------------------------------
    std::string newLog = scripting->ConsumeOutputLog();
    if (!newLog.empty())
        m_pythonOutputLog += newLog;

    ImGui::Separator();
    ImGui::TextUnformatted("Captured stdout/stderr:");
    ImGui::SameLine();
    if (ImGui::SmallButton("clear log"))
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

