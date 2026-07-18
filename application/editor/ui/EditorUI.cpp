#include "ui/EditorUIInternal.h"
#include <engine/RenderSessionApi.h>
#include <engine/App.h>

#include "SceneEditor.h"
#include "common/ImGuiManager.h"
#include "common/TransformGizmo.h"

#include <render/AppDiagnostics.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/Scene.h>
#include <imgui_internal.h>
#include <cstdio>
#include <render/passes/debug/Korgi.h>
#include <common/CaptureScriptManager.h>

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

void InitializeEditorUIDataFromCommandLine(EditorUIData& ui, const CommandLineOptions& cmdLine)
{
    caustica::render::InitializeRenderAppStateFromCommandLine(ui.render, cmdLine);
}

EditorUI::EditorUI(GpuDevice* deviceManager, SceneEditor& sceneEditor, EditorUIData& ui, bool NVAPI_SERSupported, const CommandLineOptions& cmdLine)
        : ImGui_Renderer(deviceManager)
        , m_sceneEditor(sceneEditor)
        , m_ui(ui)
        , m_settings(ui.render.settings)
        , m_runtime(ui.render.runtime)
        , m_editorUI(ui.editor)
        , m_NVAPI_SERSupported(NVAPI_SERSupported)
{
    m_commandList = getDevice()->createCommandList();

    // ImGui lifecycle management (fonts, context config, extensions)
    m_imguiManager = std::make_unique<ImGuiManager>(m_ui, cmdLine, NVAPI_SERSupported);
    m_imguiManager->loadDefaultFont(*this, getLocalPath(c_AssetsFolder));

    // Choose which, if any, hit object extension we can use
    m_imguiManager->configureExtensions((int)getDevice()->getGraphicsAPI());

    // apply command-line overrides to UI defaults
    m_imguiManager->applyCommandLineDefaults();

#if KORGI_ENABLED
    m_korgiBindings = std::make_unique<KorgiBindings>(m_ui);
#endif

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    m_ImNodesContext = ImNodes::Ez::CreateContext();
#endif
}

EditorUI::~EditorUI()
{
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    ImNodes::Ez::FreeContext(m_ImNodesContext);
#endif
}

bool EditorUI::mousePosUpdate(double xpos, double ypos)
{
    (void)xpos; (void)ypos;
    return false;
}

void EditorUI::animate(float elapsedTimeSeconds)
{
    caustica::ImGui_Renderer::animate(elapsedTimeSeconds);

    int w, h;
    getGpuDevice()->getWindowDimensions(w, h);
    ImGuiIO& io = ImGui::GetIO();

    m_showSceneWidgets = dm::clamp(m_showSceneWidgets + elapsedTimeSeconds * 8.0f * ((io.MousePos.y >= 0 && io.MousePos.y < h * 0.1f) ? (1) : (-1)), 0.0f, 1.0f);
}


void EditorUI::buildUI(void)
{
    // Non-modal product status: visible even when the settings UI is hidden.
    if (auto* diag = m_sceneEditor.app()
            ? m_sceneEditor.app()->tryResource<caustica::render::AppDiagnostics>()
            : nullptr)
    {
        const auto& warm = diag->rtPipelineWarmup;
        if (warm.active && warm.total > 0)
        {
            const char* preset = warm.currentPreset.empty() ? "..." : warm.currentPreset.data();
            char label[160];
            snprintf(
                label,
                sizeof(label),
                "RT presets ready %u/%u (%s)",
                warm.completed,
                warm.total,
                preset);
            const ImVec2 pad(12.f, 8.f);
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            const ImVec2 pos(
                ImGui::GetIO().DisplaySize.x - textSize.x - pad.x * 2.f - 16.f,
                16.f);
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddRectFilled(
                pos,
                ImVec2(pos.x + textSize.x + pad.x * 2.f, pos.y + textSize.y + pad.y * 2.f),
                IM_COL32(20, 20, 20, 180),
                4.f);
            drawList->AddText(
                ImVec2(pos.x + pad.x, pos.y + pad.y),
                IM_COL32(230, 200, 90, 255),
                label);
        }
    }

    if (!m_editorUI.ShowUI)
        return;

    RAII_SCOPE( ImGui::PushFont(m_defaultFont->getScaledFont());, ImGui::PopFont(); );

    auto& io = ImGui::GetIO();
    PanelLayout layout;
    layout.scaledWidth = io.DisplaySize.x;
    layout.scaledHeight = io.DisplaySize.y;
    layout.defWindowWidth = 335.0f * m_currentScale;
    layout.defItemWidth = layout.defWindowWidth * 0.3f * m_currentScale;
    layout.indent = (int)ImGui::GetStyle().IndentSpacing * 0.4f;

    {
        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(layout.defWindowWidth, layout.scaledHeight - 20), ImGuiCond_Appearing);

        RAII_SCOPE( ImGui::Begin("settings", 0, ImGuiWindowFlags_None /*AlwaysAutoResize*/); , ImGui::End(); );
        RAII_SCOPE( ImGui::PushItemWidth(layout.defItemWidth); , ImGui::PopItemWidth(); );

        ImGui::Text("%s, %s", getGpuDevice()->getRendererString(), caustica::resolutionInfo(*m_sceneEditor.app()).c_str() );
        ImGui::TextUnformatted(caustica::fpsInfo(*m_sceneEditor.app()).c_str());

        if (BuildUIScriptsAndEtc())
        {
            return;
        }

        BuildDisplayPerformancePanel(layout);
        BuildSystemPanel(layout);
        if (BuildSceneComboPanel(layout))
            return;
        BuildScenePanel(layout);
        BuildSampleGamePanel(layout);
        BuildCameraPanel(layout);
        BuildLightingPanel(layout);
        BuildPathTracerPanel(layout);
        BuildStochasticTextureFilteringPanel(layout);
        BuildDLSSReflexPanel(layout);
        BuildTAAPanel(layout);
        BuildRTXDIPanel(layout);
        BuildStablePlanesPanel(layout);
        BuildStandaloneDenoiserPanel(layout);
        BuildOpacityMicroMapsPanel(layout);
        BuildAccelerationStructurePanel(layout);
        BuildPostProcessPanel(layout);
        BuildDebuggingPanel(layout);
        BuildQuickToneMappingBar(layout);
    }

    BuildInspectorPanel(layout);
    BuildMaterialEditorPanel(layout);
    BuildDeltaTreeExplorerPanel(layout);
    BuildSceneWidgetsPanel(layout);
    BuildHierarchyPanel(layout);
    BuildGameStandalonePanel(layout);

    DrawTransformGizmo(TransformGizmoContext{ m_sceneEditor, m_editorUI, m_settings });
}


bool EditorUI::CheckboxUInt32(const char* label, uint32_t* v)
{
    bool pv = (*v) != 0;
    bool ret = ImGui::Checkbox(label, &pv);
    *v = pv ? (1) : (0);
    return ret;
}


} // namespace caustica::editor
