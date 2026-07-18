#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "common/TransformGizmo.h"

#include <engine/App.h>
#include <engine/RenderSessionApi.h>
#include <render/AppDiagnostics.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

namespace
{

void ApplyDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    ImGuiID dockMain = dockspaceId;
    ImGuiID dockLeft = 0;
    ImGuiID dockRight = 0;
    ImGuiID dockLeftTop = 0;
    ImGuiID dockLeftBottom = 0;
    ImGuiID dockRightTop = 0;
    ImGuiID dockRightBottom = 0;

    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.22f, &dockLeft, &dockMain);
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.24f, &dockRight, &dockMain);
    ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.45f, &dockLeftBottom, &dockLeftTop);
    ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.45f, &dockRightBottom, &dockRightTop);

    ImGui::DockBuilderDockWindow("Render Settings", dockLeftTop);
    ImGui::DockBuilderDockWindow("Hierarchy", dockLeftBottom);
    ImGui::DockBuilderDockWindow("Viewport", dockMain);
    ImGui::DockBuilderDockWindow("Inspector", dockRightTop);
    ImGui::DockBuilderDockWindow("Material Editor", dockRightBottom);

    ImGui::DockBuilderFinish(dockspaceId);
}

} // namespace

void EditorUI::BuildMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Reload Shaders", "F5"))
        {
            // Triggered via existing script/system panels when available.
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4"))
            m_sceneEditor.app()->requestExit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        ImGui::MenuItem("Transform Gizmo", nullptr, &m_editorUI.ShowTransformGizmo);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Viewport", nullptr, &m_editorUI.Viewport.ShowViewport);
        ImGui::MenuItem("Hierarchy", nullptr, &m_editorUI.Viewport.ShowHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &m_editorUI.ShowInspector);
        ImGui::MenuItem("Material Editor", nullptr, &m_editorUI.ShowMaterialEditor);
        ImGui::MenuItem("Render Settings", nullptr, &m_editorUI.Viewport.ShowRenderSettings);
        ImGui::MenuItem("Status Bar", nullptr, &m_editorUI.Viewport.ShowStatusBar);
        ImGui::Separator();
        ImGui::MenuItem("Show All UI", "F2", &m_editorUI.ShowUI);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window"))
    {
        if (ImGui::MenuItem("Reset Window Layout"))
        {
            m_editorUI.Viewport.ShowViewport = true;
            m_editorUI.Viewport.ShowHierarchy = true;
            m_editorUI.ShowInspector = true;
            m_editorUI.ShowMaterialEditor = true;
            m_editorUI.Viewport.ShowRenderSettings = true;
            m_editorUI.Viewport.RequestResetDockLayout = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void EditorUI::BuildDockSpace()
{
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable))
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float statusH = (m_editorUI.ShowUI && m_editorUI.Viewport.ShowStatusBar)
        ? ImGui::GetFrameHeight()
        : 0.f;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, std::max(1.f, viewport->WorkSize.y - statusH)));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));

    ImGui::Begin("##EditorDockSpaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Bump id when default dock window set changes so old ini layouts migrate.
    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace_v3");
    if (m_editorUI.Viewport.RequestResetDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
    {
        ApplyDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
        m_editorUI.Viewport.RequestResetDockLayout = false;
    }

    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    // Keep dock layout alive while chrome is hidden (F2), otherwise windows undock.
    if (!m_editorUI.ShowUI)
        dockFlags |= ImGuiDockNodeFlags_KeepAliveOnly;

    ImGui::DockSpace(dockspaceId, ImVec2(0.f, 0.f), dockFlags);
    ImGui::End();
}

void EditorUI::BuildStatusBar()
{
    if (!m_editorUI.Viewport.ShowStatusBar)
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 4.f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##StatusBar", nullptr, flags))
    {
        App* app = m_sceneEditor.app();
        if (app)
        {
            ImGui::TextUnformatted(getGpuDevice()->getRendererString());
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextUnformatted(caustica::resolutionInfo(*app).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextUnformatted(caustica::fpsInfo(*app).c_str());

            if (auto* diag = app->tryResource<caustica::render::AppDiagnostics>())
            {
                const auto& warm = diag->rtPipelineWarmup;
                if (warm.active && warm.total > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextColored(GetEditorColors().TextWarning, "RT %u/%u", warm.completed, warm.total);
                }
            }

            if (m_editorUI.Viewport.RectValid)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("Viewport %ux%u", m_editorUI.Viewport.DesiredWidth, m_editorUI.Viewport.DesiredHeight);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void EditorUI::BuildViewportPanel(const PanelLayout& layout)
{
    (void)layout;
    auto& vp = m_editorUI.Viewport;
    if (!vp.ShowViewport)
    {
        vp.RectValid = false;
        return;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;

    // Zero padding so the render fills the dock node edge-to-edge (no grey gutters).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    const bool open = ImGui::Begin("Viewport", &vp.ShowViewport, flags);
    ImGui::PopStyleVar();
    if (!open)
    {
        vp.RectValid = false;
        ImGui::End();
        return;
    }

    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(1.f, canvasSize.x);
    canvasSize.y = std::max(1.f, canvasSize.y);

    vp.PosX = canvasPos.x;
    vp.PosY = canvasPos.y;
    vp.SizeX = canvasSize.x;
    vp.SizeY = canvasSize.y;
    vp.DesiredWidth = static_cast<uint32_t>(canvasSize.x + 0.5f);
    vp.DesiredHeight = static_cast<uint32_t>(canvasSize.y + 0.5f);
    vp.RectValid = true;

    nvrhi::ITexture* color = m_viewportColor;
    ImGui::InvisibleButton("##ViewportCanvas", canvasSize);
    vp.Hovered = ImGui::IsItemHovered();
    vp.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (color)
    {
        ImTextureRef tex(color);
        drawList->AddImage(
            tex,
            canvasPos,
            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y));
    }
    else
    {
        drawList->AddRectFilled(
            canvasPos,
            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
            IM_COL32(20, 22, 26, 255));
        const char* msg = "Waiting for viewport...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        drawList->AddText(
            ImVec2(canvasPos.x + (canvasSize.x - ts.x) * 0.5f, canvasPos.y + (canvasSize.y - ts.y) * 0.5f),
            IM_COL32(160, 160, 160, 255),
            msg);
    }

    // Overlay toolbar on the image (does not reserve layout space / create gutters).
    {
        const float pad = 6.f;
        ImGui::SetCursorScreenPos(ImVec2(canvasPos.x + pad, canvasPos.y + pad));
        ImGui::BeginGroup();
        BuildTransformGizmoToolbar(m_editorUI);
        ImGui::EndGroup();
    }

    ImGui::End();
}

} // namespace caustica::editor
