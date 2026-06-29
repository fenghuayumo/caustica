#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorApplication.h"
#include "common/ImGuiManager.h"

#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneGraph.h>
#include <imgui_internal.h>
#include <render/Passes/Debug/Korgi.h>
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
    caustica::render::InitializeRenderSessionStateFromCommandLine(ui, cmdLine);
}

EditorUI::EditorUI(GpuDevice* deviceManager, EditorApplication& editor, EditorUIData& ui, bool NVAPI_SERSupported, const CommandLineOptions& cmdLine)
        : ImGui_Renderer(deviceManager)
        , m_app(editor)
        , m_sceneEditor(editor.GetSceneEditor())
        , m_ui(ui)
        , m_NVAPI_SERSupported(NVAPI_SERSupported)
{
    m_commandList = GetDevice()->createCommandList();

    // ImGui lifecycle management (fonts, context config, extensions)
    m_imguiManager = std::make_unique<ImGuiManager>(m_ui, cmdLine, NVAPI_SERSupported);
    m_imguiManager->loadDefaultFont(*this, GetLocalPath(c_AssetsFolder));

    // Choose which, if any, hit object extension we can use
    m_imguiManager->configureExtensions((int)GetDevice()->getGraphicsAPI());

    // Apply command-line overrides to UI defaults
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

bool EditorUI::MousePosUpdate(double xpos, double ypos)
{
    (void)xpos; (void)ypos;
    return false;
}

void EditorUI::Animate(float elapsedTimeSeconds)
{
    caustica::ImGui_Renderer::Animate(elapsedTimeSeconds);

    int w, h;
    GetGpuDevice()->GetWindowDimensions(w, h);
    ImGuiIO& io = ImGui::GetIO();

    m_showSceneWidgets = dm::clamp(m_showSceneWidgets + elapsedTimeSeconds * 8.0f * ((io.MousePos.y >= 0 && io.MousePos.y < h * 0.1f) ? (1) : (-1)), 0.0f, 1.0f);
}


void EditorUI::buildUI(void)
{
    if (!m_ui.ShowUI)
        return;

    RAII_SCOPE( ImGui::PushFont(m_defaultFont->GetScaledFont());, ImGui::PopFont(); );

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

        RAII_SCOPE( ImGui::Begin("Settings", 0, ImGuiWindowFlags_None /*AlwaysAutoResize*/); , ImGui::End(); );
        RAII_SCOPE( ImGui::PushItemWidth(layout.defItemWidth); , ImGui::PopItemWidth(); );

        ImGui::Text("%s, %s", GetGpuDevice()->GetRendererString(), m_sceneEditor.GetResolutionInfo().c_str() );
        ImGui::Text("%s", m_sceneEditor.GetFPSInfo().c_str());

        if (BuildUIScriptsAndEtc())
        {
            return;
        }

        BuildDisplayPerformancePanel(layout);
        BuildSystemPanel(layout);
        BuildSceneComboPanel(layout);
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
}


bool EditorUI::CheckboxUInt32(const char* label, uint32_t* v)
{
    bool pv = (*v) != 0;
    bool ret = ImGui::Checkbox(label, &pv);
    *v = pv ? (1) : (0);
    return ret;
}


} // namespace caustica::editor
