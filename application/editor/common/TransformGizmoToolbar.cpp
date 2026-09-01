#include "common/TransformGizmo.h"
#include "common/EditorTheme.h"
#include "common/IconsMaterialSymbols.h"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace caustica::editor
{
namespace
{

// Match vk_gaussian_splatting toolbar: Material Symbols glyphs in compact toggle buttons.
constexpr float kBtn = 28.f;
constexpr float kPad = 3.f;
constexpr float kGap = 2.f;
constexpr float kRound = 3.f;

void PushIconStyle(bool active)
{
    // Active uses caustica accent blue; idle uses vkgs-like gray chrome.
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.40f, 0.62f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.50f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.32f, 0.52f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
    }
}

void PopIconStyle()
{
    ImGui::PopStyleColor(3);
}

bool ToolButton(const char* id, const char* iconUtf8, bool selected, const char* tip)
{
    PushIconStyle(selected);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 4.f));
    ImGui::PushID(id);
    const bool pressed = ImGui::Button(iconUtf8, ImVec2(kBtn, kBtn));
    ImGui::PopID();
    ImGui::PopStyleVar(2);
    PopIconStyle();

    if (tip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

void ToolbarSeparator(ImDrawList* dl, float height)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + 1.5f;
    dl->AddLine(
        ImVec2(x, p.y + 5.f),
        ImVec2(x, p.y + height - 5.f),
        IM_COL32(255, 255, 255, 32),
        1.0f);
    ImGui::Dummy(ImVec2(5.f, height));
}

} // namespace

void caustica::editor::GetTransformGizmoToolbarSize(float& width, float& height)
{
    // Select | Move | Rotate | Scale | Space | Snap | Grid | Lights | Material
    constexpr int toolCount = 9;
    constexpr int gapCount = 6;
    constexpr int sepCount = 2;
    width = kPad * 2.f + kBtn * float(toolCount) + kGap * float(gapCount) + 5.f * float(sepCount);
    height = kBtn + kPad * 2.f;
}

void caustica::editor::BuildTransformGizmoToolbar(EditorUIState& editorUI)
{
    const auto operation = static_cast<ImGuizmo::OPERATION>(editorUI.GizmoOperation);
    const auto mode = static_cast<ImGuizmo::MODE>(editorUI.GizmoMode);
    const bool selectMode = !editorUI.GizmoEnabled;
    const bool isTranslate = !selectMode && (operation == ImGuizmo::TRANSLATE);
    const bool isRotate = !selectMode && (operation == ImGuizmo::ROTATE);
    const bool isScale = !selectMode && (operation == ImGuizmo::SCALE);
    const bool isLocal = (mode == ImGuizmo::LOCAL);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    float stripW = 0.f;
    float stripH = 0.f;
    GetTransformGizmoToolbarSize(stripW, stripH);

    dl->AddRectFilled(
        origin,
        ImVec2(origin.x + stripW, origin.y + stripH),
        IM_COL32(22, 24, 28, 210),
        6.f);
    dl->AddRect(
        origin,
        ImVec2(origin.x + stripW, origin.y + stripH),
        IM_COL32(255, 255, 255, 24),
        6.f,
        0,
        1.0f);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + kPad, origin.y + kPad));
    ImGui::BeginGroup();

    if (ToolButton("##GizmoSelect", ICON_MS_ARROW_SELECTOR_TOOL, selectMode, "Select (Q)"))
        editorUI.GizmoEnabled = false;
    ImGui::SameLine(0.f, kGap);

    // open_with = four-way move arrows; autorenew = circular rotate arrows (Blender-like).
    if (ToolButton("##GizmoTranslate", ICON_MS_OPEN_WITH, isTranslate, "Translate (T)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoRotate", ICON_MS_AUTORENEW, isRotate, "Rotate (R)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoScale", ICON_MS_ZOOM_OUT_MAP, isScale, "Scale (S)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);
    }

    ImGui::SameLine(0.f, 0.f);
    ToolbarSeparator(dl, kBtn);
    ImGui::SameLine(0.f, 0.f);

    if (ToolButton(
            "##GizmoSpace",
            isLocal ? ICON_MS_VIEW_IN_AR : ICON_MS_PUBLIC,
            false,
            isLocal ? "Local space (click for World)" : "World space (click for Local)"))
    {
        editorUI.GizmoMode = static_cast<int>(
            isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoSnap", ICON_MS_ATTRACTIONS, editorUI.GizmoSnapEnabled, "Snap"))
        editorUI.GizmoSnapEnabled = !editorUI.GizmoSnapEnabled;

    ImGui::SameLine(0.f, 0.f);
    ToolbarSeparator(dl, kBtn);
    ImGui::SameLine(0.f, 0.f);

    if (ToolButton(
            "##GizmoGrid",
            ICON_MS_GRID_ON,
            editorUI.ShowInfiniteGrid,
            editorUI.ShowInfiniteGrid ? "Hide infinite grid" : "Show infinite grid"))
        editorUI.ShowInfiniteGrid = !editorUI.ShowInfiniteGrid;

    ImGui::SameLine(0.f, kGap);

    if (ToolButton(
            "##GizmoLights",
            editorUI.ShowLightHelpers ? ICON_MS_VISIBILITY : ICON_MS_VISIBILITY_OFF,
            editorUI.ShowLightHelpers,
            editorUI.ShowLightHelpers ? "Hide light gizmos (G)" : "Show light gizmos (G)"))
        editorUI.ShowLightHelpers = !editorUI.ShowLightHelpers;

    ImGui::SameLine(0.f, kGap);

    if (ToolButton(
            "##MaterialPicker",
            ICON_MS_COLORIZE,
            editorUI.MaterialPickerActive,
            editorUI.MaterialPickerActive
                ? "Cancel material picker"
                : "Pick material from viewport"))
        editorUI.MaterialPickerActive = !editorUI.MaterialPickerActive;

    ImGui::EndGroup();
}

} // namespace caustica::editor
