#include "ui/EditorUIInternal.h"
#include <engine/RenderSessionApi.h>
#include <engine/App.h>

#include "SceneEditor.h"
#include "common/ImGuiManager.h"
#include "common/IconsMaterialSymbols.h"
#include "common/TransformGizmo.h"
#include "ui/RenderSettingsConsole.h"

#include <render/AppDiagnostics.h>
#include <core/vfs/VFS.h>
#include <imgui_internal.h>
#include <platform/window.h>
#include <core/path_utils.h>
#include <algorithm>
#include <cstdio>
#include <render/passes/debug/Korgi.h>
#include <common/CaptureScriptManager.h>
#include <core/console/ConsoleInterpreter.h>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

void InitializeEditorUIDataFromCommandLine(EditorUIData& ui, const CommandLineOptions& cmdLine)
{
    caustica::render::InitializeRenderAppStateFromCommandLine(ui.render, cmdLine);
}

EditorUI::EditorUI(
    GpuDevice* device,
    SceneEditor& sceneEditor,
    EditorUIData& ui,
    bool NVAPI_SERSupported,
    const CommandLineOptions& cmdLine,
    RenderSettingsConsoleBinding& console)
        : ImGui_Renderer(device)
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
    m_defaultStyle = ImGui::GetStyle();

    // Choose which, if any, hit object extension we can use
    m_imguiManager->configureExtensions((int)getDevice()->getGraphicsAPI());

    // apply command-line overrides to UI defaults
    m_imguiManager->applyCommandLineDefaults();

    caustica::ImGui_Console::Options consoleOptions;
    consoleOptions.capture_log = true;
    consoleOptions.show_info = true;
    m_console = std::make_unique<caustica::ImGui_Console>(
        console.interpreter(), consoleOptions);

#if KORGI_ENABLED
    m_korgiBindings = std::make_unique<KorgiBindings>(m_ui);
#endif

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    m_ImNodesContext = ImNodes::Ez::CreateContext();
#endif
}

EditorUI::~EditorUI()
{
    if (m_materialPickerCursorVisible)
    {
        if (Window* window = getGpuDevice() && getGpuDevice()->surface()
                ? getGpuDevice()->surface()->window()
                : nullptr)
            window->setCursorVisible(true);
    }
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    ImNodes::Ez::FreeContext(m_ImNodesContext);
#endif
}

void EditorUI::UpdateMaterialPickerCursor()
{
    const auto& viewport = m_editorUI.Viewport;
    const bool visible = m_editorUI.MaterialPickerActive
        && m_editorUI.ShowUI
        && viewport.ShowViewport
        && viewport.RectValid
        && viewport.Hovered;

    Window* window = getGpuDevice() && getGpuDevice()->surface()
        ? getGpuDevice()->surface()->window()
        : nullptr;
    if (window && visible != m_materialPickerCursorVisible)
        window->setCursorVisible(!visible);
    m_materialPickerCursorVisible = visible;

    if (!visible)
        return;

    // Draw the same Material Symbols eyedropper used by the toolbar. The real
    // cursor is hidden only over the viewport; the glyph's top-left is the pick hotspot.
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 glyphPos(mouse.x + 2.f, mouse.y + 2.f);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * 1.4f;
    drawList->AddText(
        font,
        fontSize,
        ImVec2(glyphPos.x + 1.f, glyphPos.y + 1.f),
        IM_COL32(0, 0, 0, 220),
        ICON_MS_COLORIZE);
    drawList->AddText(
        font,
        fontSize,
        glyphPos,
        IM_COL32(235, 242, 255, 255),
        ICON_MS_COLORIZE);
}

bool EditorUI::mousePosUpdate(double xpos, double ypos)
{
    (void)xpos; (void)ypos;
    return false;
}

void EditorUI::displayScaleChanged(float scaleX, float scaleY)
{
    // Match historical EditorUI behavior: track scale only.
    // Do not clear fonts (base ImGui_Renderer path) and do not mutate ImGuiStyle
    // here — theme is applied once at ImGuiManager construction.
    m_currentScale = scaleX;
    assert(scaleX == scaleY);
}

void EditorUI::animate(float elapsedTimeSeconds)
{
    caustica::ImGui_Renderer::animate(elapsedTimeSeconds);
}


void EditorUI::buildUI(void)
{
    // Non-modal product status: visible even when the settings UI is hidden.
    if (auto* diag = m_sceneEditor.app()
            ? m_sceneEditor.app()->tryResource<caustica::render::AppDiagnostics>()
            : nullptr)
    {
        const auto warm = diag->pipelineWarmupStatus();
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

    BuildMainMenuBar();
    BuildDockSpace();
    BuildAboutPanel();

    if (!m_editorUI.ShowUI)
    {
        // Command bar stays available with UI chrome hidden (F2).
        if (m_editorUI.ShowCommandBar && m_console)
        {
            const bool focusCommandBar = m_editorUI.RequestFocusCommandBar;
            m_editorUI.RequestFocusCommandBar = false;
            const auto& vp = m_editorUI.Viewport;
            m_console->renderCommandBar(
                &m_editorUI.ShowCommandBar,
                focusCommandBar,
                vp.RectValid ? ImVec2(vp.PosX, vp.PosY) : ImVec2(0.f, 0.f),
                vp.RectValid ? ImVec2(vp.SizeX, vp.SizeY) : ImVec2(0.f, 0.f));
        }
        BuildStatusBar();
        UpdateMaterialPickerCursor();
        return;
    }

    RAII_SCOPE( ImGui::PushFont(m_defaultFont->getScaledFont());, ImGui::PopFont(); );

    auto& io = ImGui::GetIO();
    PanelLayout layout;
    layout.scaledWidth = io.DisplaySize.x;
    layout.scaledHeight = io.DisplaySize.y;
    layout.defWindowWidth = 335.0f * m_currentScale;
    layout.defItemWidth = layout.defWindowWidth * 0.3f * m_currentScale;
    layout.indent = (int)ImGui::GetStyle().IndentSpacing * 0.4f;

    BuildViewportPanel(layout);

    if (m_editorUI.Viewport.ShowRenderSettings)
    {
        RAII_SCOPE(ImGui::Begin("Render Settings", &m_editorUI.Viewport.ShowRenderSettings);, ImGui::End(););
        RAII_SCOPE(ImGui::PushItemWidth(layout.defItemWidth);, ImGui::PopItemWidth(););

        if (BuildUIScriptsAndEtc())
        {
            BuildStatusBar();
            UpdateMaterialPickerCursor();
            return;
        }

        const char* detailLabels[] = { "Basic", "Advanced" };
        const float segmentSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float segmentWidth = std::max(
            1.f,
            (ImGui::GetContentRegionAvail().x
                - segmentSpacing * (IM_ARRAYSIZE(detailLabels) - 1))
                / IM_ARRAYSIZE(detailLabels));
        for (int i = 0; i < IM_ARRAYSIZE(detailLabels); ++i)
        {
            if (i > 0)
                ImGui::SameLine(0.f, segmentSpacing);

            const bool selected =
                m_editorUI.ShowAdvancedRenderSettings == (i == 1);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton(
                    "##RenderSettingsDetail",
                    ImVec2(segmentWidth, ImGui::GetFrameHeight())))
            {
                m_editorUI.ShowAdvancedRenderSettings = i == 1;
            }

            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const ImU32 background = ImGui::GetColorU32(
                selected
                    ? ImGuiCol_Header
                    : (ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_Button));
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(
                itemMin,
                itemMax,
                background,
                ImGui::GetStyle().FrameRounding);
            const ImVec2 textSize = ImGui::CalcTextSize(detailLabels[i]);
            drawList->AddText(
                ImVec2(
                    itemMin.x + (itemMax.x - itemMin.x - textSize.x) * 0.5f,
                    itemMin.y + (itemMax.y - itemMin.y - textSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text),
                detailLabels[i]);
            ImGui::PopID();
        }
        ImGui::Separator();

        BuildRenderSettingsOverview(layout);
        if (m_editorUI.ShowAdvancedRenderSettings)
            BuildAdvancedRenderSettings(layout);
    }

    BuildPreferencesPanel(layout);
    BuildInspectorPanel(layout);
    BuildMaterialEditorPanel(layout);
    BuildPostProcessPanel(layout);
    BuildDeltaTreeExplorerPanel(layout);
    if (m_editorUI.Viewport.ShowHierarchy)
        BuildHierarchyPanel(layout);
    BuildGameStandalonePanel(layout);
    BuildTimelinePanel(layout);
    if (m_editorUI.ShowConsole && m_console)
    {
        const bool focusConsole = m_editorUI.RequestFocusConsole;
        m_editorUI.RequestFocusConsole = false;
        m_console->render(&m_editorUI.ShowConsole, focusConsole);
    }

    // After all dock panels (same ordering as pre-DockSpace): ImGuizmo BeginFrame +
    // foreground draw list so the gizmo is never covered by the Viewport image.
    const TransformGizmoContext gizmoCtx{ m_sceneEditor, m_editorUI, m_settings };
    DrawInfiniteGrid(gizmoCtx);
    DrawLightHelpers(gizmoCtx);
    DrawTransformGizmo(gizmoCtx);
    DrawViewOrientationGizmo(gizmoCtx);
    BuildStatusBar();

    // Draw last so the UE-style command bar stays above docked panels,
    // and clamp it to the Viewport rect (not under Hierarchy / Inspector).
    if (m_editorUI.ShowCommandBar && m_console)
    {
        const bool focusCommandBar = m_editorUI.RequestFocusCommandBar;
        m_editorUI.RequestFocusCommandBar = false;
        const auto& vp = m_editorUI.Viewport;
        m_console->renderCommandBar(
            &m_editorUI.ShowCommandBar,
            focusCommandBar,
            vp.RectValid ? ImVec2(vp.PosX, vp.PosY) : ImVec2(0.f, 0.f),
            vp.RectValid ? ImVec2(vp.SizeX, vp.SizeY) : ImVec2(0.f, 0.f));
    }

    UpdateMaterialPickerCursor();
}


bool EditorUI::CheckboxUInt32(const char* label, uint32_t* v)
{
    bool pv = (*v) != 0;
    bool ret = ImGui::Checkbox(label, &pv);
    *v = pv ? (1) : (0);
    return ret;
}

} // namespace caustica::editor
