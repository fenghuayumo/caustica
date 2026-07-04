#include "ui/ui_macros.h"
#pragma once

#include <math/math.h>
#include <scene/Scene.h>

#include <imgui/imgui_renderer.h>
#include <imgui/imgui_console.h>

#include <render/Passes/RTXDI/RtxdiPass.h>

#include <render/Passes/Geometry/TemporalAntiAliasingPass.h>

using namespace caustica::math;

#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
#include "ImNodesEz.h"
#endif

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

#include <render/Passes/Denoisers/NrdConfig.h>
#include <render/Core/PathTracerSettings.h>
#include <render/Passes/Debug/Korgi.h>
#include "EditorUIData.h"
#include <core/command_line.h>

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
#include <Rtxtf/STFDefinitions.h>
#endif

namespace caustica::editor
{

class SceneEditor;
class ImGuiManager;
class EditorApplication;

class EditorUI : public caustica::ImGui_Renderer
{
public:
    EditorUI(caustica::GpuDevice* deviceManager, EditorApplication& editor, EditorUIData& ui, bool NVAPI_SERSupported, const CommandLineOptions& cmdLine);
    virtual ~EditorUI();
protected:
    virtual void buildUI(void) override;
private:
    void buildDeltaTreeViz();

    virtual bool MousePosUpdate(double xpos, double ypos);
    virtual void DisplayScaleChanged(float scaleX, float scaleY) override { m_currentScale = scaleX; assert( scaleX == scaleY ); }
    virtual void Animate(float elapsedTimeSeconds) override;

    bool BuildUIScriptsAndEtc(void);
    void BuildUIResolutionPicker();
    void BuildUIPerformancePresets();
    
    void DLSSFGSelectorUI();

private:
    void BuildPythonScriptingUI(float indent);

    // --- Panel layout context (computed once per frame in buildUI, passed to each panel) ---
    struct PanelLayout
    {
        float indent = 0.f;
        float defItemWidth = 0.f;
        float defWindowWidth = 0.f;
        float scaledWidth = 0.f;
        float scaledHeight = 0.f;
    };

    bool CheckboxUInt32(const char* label, uint32_t* v);

    // Settings window collapsing-header panels
    void BuildDisplayPerformancePanel(const PanelLayout& layout);
    void BuildSystemPanel(const PanelLayout& layout);
    bool BuildSceneComboPanel(const PanelLayout& layout);
    void BuildScenePanel(const PanelLayout& layout);
    void BuildSampleGamePanel(const PanelLayout& layout);
    void BuildCameraPanel(const PanelLayout& layout);
    void BuildLightingPanel(const PanelLayout& layout);
    void BuildPathTracerPanel(const PanelLayout& layout);
    void BuildStochasticTextureFilteringPanel(const PanelLayout& layout);
    void BuildDLSSReflexPanel(const PanelLayout& layout);
    void BuildTAAPanel(const PanelLayout& layout);
    void BuildRTXDIPanel(const PanelLayout& layout);
    void BuildStablePlanesPanel(const PanelLayout& layout);
    void BuildStandaloneDenoiserPanel(const PanelLayout& layout);
    void BuildOpacityMicroMapsPanel(const PanelLayout& layout);
    void BuildAccelerationStructurePanel(const PanelLayout& layout);
    void BuildPostProcessPanel(const PanelLayout& layout);
    void BuildDebuggingPanel(const PanelLayout& layout);
    void BuildQuickToneMappingBar(const PanelLayout& layout);
    // Standalone windows / overlays
    void BuildInspectorPanel(const PanelLayout& layout);
    void BuildMaterialEditorPanel(const PanelLayout& layout);
    void BuildDeltaTreeExplorerPanel(const PanelLayout& layout);
    void BuildSceneWidgetsPanel(const PanelLayout& layout);
    void BuildHierarchyPanel(const PanelLayout& layout);
    void BuildGameStandalonePanel(const PanelLayout& layout);

private:
    EditorApplication& m_app;
    SceneEditor& m_sceneEditor;

    int                         m_currentFontScaleIndex = -1;
    float                       m_currentScale = 1.0f;
    ImGuiStyle                  m_defaultStyle;

    // Embedded Python scripting UI state
    std::string                 m_pythonScriptPath;     // Last loaded path
    std::string                 m_pythonInlineCode;     // Editable inline expression
    std::string                 m_pythonOutputLog;      // Captured stdout/stderr from scripts

    float                       m_showSceneWidgets = 0.0f;

    std::unique_ptr<caustica::ImGui_Console> m_console;
    std::shared_ptr<caustica::Light> m_SelectedLight;

    std::unique_ptr<ImGuiManager> m_imguiManager;

    EditorUIData& m_ui;
    PathTracerSettings& m_settings;
    caustica::render::RenderRuntimeState& m_runtime;
    EditorUIState& m_editorUI;
    nvrhi::CommandListHandle m_commandList;

    const bool m_NVAPI_SERSupported;

#if KORGI_ENABLED
    struct KorgiBindings
    {
        korgi::Button play;
        korgi::Button autoExposure;
        korgi::Button toneMapOperator;
        korgi::Knob   exposureCompensation;

        explicit KorgiBindings(EditorUIData& ui)
            : play(0, korgi::Control::Play, korgi::ButtonMode::BoolToggle, &ui.session.settings.EnableAnimations)
            , autoExposure(0, korgi::Control::S1, korgi::ButtonMode::BoolToggle, &ui.session.settings.ToneMappingParams.autoExposure)
            , toneMapOperator(0, korgi::Control::M1, (int*)&ui.session.settings.ToneMappingParams.toneMapOperator, int(ToneMapperOperator::Linear), int(ToneMapperOperator::HableUc2))
            , exposureCompensation(0, korgi::Control::Slider1, &ui.session.settings.ToneMappingParams.exposureCompensation, -8.f, 8.f)
        {}
    };
    std::unique_ptr<KorgiBindings> m_korgiBindings;
#endif

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    ImNodes::Ez::Context* m_ImNodesContext;
#endif
};

void UpdateTogglableNodes(std::vector<TogglableNode>& togglableNodes, caustica::scene::SceneEntityWorld& entityWorld, ecs::Entity entity);

} // namespace caustica::editor
