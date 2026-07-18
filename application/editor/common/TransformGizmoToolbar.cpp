#include "common/TransformGizmo.h"
#include "common/EditorTheme.h"

#include <ImGuizmo.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace caustica::editor
{
namespace
{

void DrawGizmoIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, int kind, ImU32 col)
{
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float s = std::min(max.x - min.x, max.y - min.y) * 0.32f;
    const float t = std::max(1.5f, s * 0.18f);

    if (kind == 0) // Move
    {
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s), col, t);
        dl->AddTriangleFilled(ImVec2(c.x + s, c.y), ImVec2(c.x + s - s * 0.45f, c.y - s * 0.3f), ImVec2(c.x + s - s * 0.45f, c.y + s * 0.3f), col);
        dl->AddTriangleFilled(ImVec2(c.x - s, c.y), ImVec2(c.x - s + s * 0.45f, c.y - s * 0.3f), ImVec2(c.x - s + s * 0.45f, c.y + s * 0.3f), col);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - s), ImVec2(c.x - s * 0.3f, c.y - s + s * 0.45f), ImVec2(c.x + s * 0.3f, c.y - s + s * 0.45f), col);
        dl->AddTriangleFilled(ImVec2(c.x, c.y + s), ImVec2(c.x - s * 0.3f, c.y + s - s * 0.45f), ImVec2(c.x + s * 0.3f, c.y + s - s * 0.45f), col);
    }
    else if (kind == 1) // Rotate
    {
        dl->PathClear();
        dl->PathArcTo(c, s, -2.4f, 1.8f, 16);
        dl->PathStroke(col, 0, t);
        const float a = 1.8f;
        const ImVec2 tip(c.x + cosf(a) * s, c.y + sinf(a) * s);
        dl->AddTriangleFilled(
            tip,
            ImVec2(tip.x + cosf(a + 2.2f) * s * 0.4f, tip.y + sinf(a + 2.2f) * s * 0.4f),
            ImVec2(tip.x + cosf(a + 0.9f) * s * 0.4f, tip.y + sinf(a + 0.9f) * s * 0.4f),
            col);
    }
    else // Scale
    {
        const float e = s * 0.85f;
        dl->AddLine(ImVec2(c.x - e, c.y - e), ImVec2(c.x - e * 0.25f, c.y - e), col, t);
        dl->AddLine(ImVec2(c.x - e, c.y - e), ImVec2(c.x - e, c.y - e * 0.25f), col, t);
        dl->AddLine(ImVec2(c.x + e, c.y + e), ImVec2(c.x + e * 0.25f, c.y + e), col, t);
        dl->AddLine(ImVec2(c.x + e, c.y + e), ImVec2(c.x + e, c.y + e * 0.25f), col, t);
        dl->AddLine(ImVec2(c.x - e * 0.35f, c.y + e * 0.35f), ImVec2(c.x + e * 0.35f, c.y - e * 0.35f), col, t);
    }
}

bool IconToolButton(const char* id, int iconKind, bool selected, const char* tip)
{
    const ImVec2 size(28.f, 28.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool pressed = ImGui::InvisibleButton(id, size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        selected ? GetEditorColors().Accent : GetEditorColors().ToolbarIdle);
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(GetEditorColors().Text);
    dl->AddRectFilled(p0, p1, bg, 2.f);
    if (ImGui::IsItemHovered())
        dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(GetEditorColors().AccentHovered), 2.f, 0, 1.25f);
    DrawGizmoIcon(dl, p0, p1, iconKind, fg);

    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tip);

    return pressed;
}

} // namespace

void BuildTransformGizmoToolbar(EditorUIState& editorUI)
{
    const auto operation = static_cast<ImGuizmo::OPERATION>(editorUI.GizmoOperation);
    const auto mode = static_cast<ImGuizmo::MODE>(editorUI.GizmoMode);

    if (IconToolButton("##GizmoMove", 0, operation == ImGuizmo::TRANSLATE, "Move (W)"))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    ImGui::SameLine(0.f, 4.f);
    if (IconToolButton("##GizmoRotate", 1, operation == ImGuizmo::ROTATE, "Rotate (E)"))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    ImGui::SameLine(0.f, 4.f);
    if (IconToolButton("##GizmoScale", 2, operation == ImGuizmo::SCALE, "Scale (T)"))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);

    ImGui::SameLine(0.f, 12.f);

    if (operation != ImGuizmo::SCALE)
    {
        PushToolbarButtonColors(mode == ImGuizmo::LOCAL);
        if (ImGui::SmallButton("Local"))
            editorUI.GizmoMode = static_cast<int>(ImGuizmo::LOCAL);
        PopToolbarButtonColors();
        ImGui::SameLine(0.f, 4.f);
        PushToolbarButtonColors(mode == ImGuizmo::WORLD);
        if (ImGui::SmallButton("World"))
            editorUI.GizmoMode = static_cast<int>(ImGuizmo::WORLD);
        PopToolbarButtonColors();
        ImGui::SameLine(0.f, 12.f);
    }

    ImGui::Checkbox("Snap", &editorUI.GizmoSnapEnabled);
}

} // namespace caustica::editor
