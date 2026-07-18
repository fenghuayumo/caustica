#include "common/TransformGizmo.h"
#include "common/EditorTheme.h"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace caustica::editor
{
namespace
{

// SuperSplat-style compact floating tool strip: white silhouette glyphs on dark chrome.
constexpr float kBtn = 28.f;
constexpr float kPad = 3.f;
constexpr float kGap = 1.f;
constexpr float kRound = 4.f;

ImU32 Col(const ImVec4& c)
{
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 Alpha(ImU32 c, float a)
{
    const int ai = static_cast<int>(a * 255.f + 0.5f);
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(ai) << 24);
}

void DrawArrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float size, ImU32 col)
{
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-4f)
        return;
    dir.x /= len;
    dir.y /= len;
    const ImVec2 n(-dir.y, dir.x);
    const ImVec2 base(tip.x - dir.x * size, tip.y - dir.y * size);
    dl->AddTriangleFilled(
        tip,
        ImVec2(base.x + n.x * size * 0.58f, base.y + n.y * size * 0.58f),
        ImVec2(base.x - n.x * size * 0.58f, base.y - n.y * size * 0.58f),
        col);
}

// --- White silhouette icons (match common DCC / SuperSplat tool glyphs) ---

void DrawSelectIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // Mouse pointer cursor
    const float h = s * 1.15f;
    const float w = s * 0.72f;
    const ImVec2 tip(c.x - w * 0.55f, c.y - h * 0.55f);
    const ImVec2 pts[7] = {
        tip,
        ImVec2(tip.x, tip.y + h),
        ImVec2(tip.x + w * 0.32f, tip.y + h * 0.72f),
        ImVec2(tip.x + w * 0.55f, tip.y + h * 1.05f),
        ImVec2(tip.x + w * 0.78f, tip.y + h * 0.92f),
        ImVec2(tip.x + w * 0.48f, tip.y + h * 0.62f),
        ImVec2(tip.x + w * 0.95f, tip.y + h * 0.55f),
    };
    dl->AddConvexPolyFilled(pts, 7, col);
}

void DrawMoveIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // 4-way translate arrows (single-color silhouette)
    const float arm = s * 0.95f;
    const float stem = std::max(1.8f, s * 0.18f);
    const float head = s * 0.42f;

    dl->AddCircleFilled(c, s * 0.16f, col, 10);
    dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), col, stem);
    dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), col, stem);
    DrawArrowHead(dl, ImVec2(c.x + arm, c.y), ImVec2(1.f, 0.f), head, col);
    DrawArrowHead(dl, ImVec2(c.x - arm, c.y), ImVec2(-1.f, 0.f), head, col);
    DrawArrowHead(dl, ImVec2(c.x, c.y - arm), ImVec2(0.f, -1.f), head, col);
    DrawArrowHead(dl, ImVec2(c.x, c.y + arm), ImVec2(0.f, 1.f), head, col);
}

void DrawRotateIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // Open circular arrow
    const float r = s * 0.88f;
    const float t = std::max(1.8f, s * 0.20f);
    const float a0 = -IM_PI * 0.75f;
    const float a1 = IM_PI * 0.85f;

    dl->PathClear();
    dl->PathArcTo(c, r, a0, a1, 28);
    dl->PathStroke(col, 0, t);

    const ImVec2 tip(c.x + cosf(a1) * r, c.y + sinf(a1) * r);
    const ImVec2 tang(-sinf(a1), cosf(a1));
    DrawArrowHead(dl, tip, tang, s * 0.48f, col);
}

void DrawScaleIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // Bounding box with outward corner arrows
    const float e = s * 0.62f;
    const float t = std::max(1.6f, s * 0.16f);
    const float box = s * 0.28f;
    const ImVec2 tl(c.x - e, c.y - e);
    const ImVec2 br(c.x + e, c.y + e);

    dl->AddRect(tl, br, col, 1.2f, 0, t);
    dl->AddRectFilled(tl, ImVec2(tl.x + box, tl.y + box), col, 1.0f);
    dl->AddRectFilled(ImVec2(br.x - box, br.y - box), br, col, 1.0f);
    DrawArrowHead(dl, ImVec2(tl.x - 0.5f, tl.y - 0.5f), ImVec2(-1.f, -1.f), s * 0.32f, col);
    DrawArrowHead(dl, ImVec2(br.x + 0.5f, br.y + 0.5f), ImVec2(1.f, 1.f), s * 0.32f, col);
}

void DrawSpaceIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool local)
{
    // Globe + cube: Local = stronger cube, World = stronger globe
    const float r = s * 0.88f;
    const float t = std::max(1.5f, s * 0.15f);

    dl->AddCircle(c, r, col, 20, t);
    dl->PathClear();
    dl->PathArcTo(c, r * 0.52f, -IM_PI * 0.5f, IM_PI * 0.5f, 12);
    dl->PathStroke(col, 0, t * 0.9f);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, t * 0.9f);

    const float hs = local ? s * 0.42f : s * 0.30f;
    const ImU32 cubeCol = local ? col : Alpha(col, 0.75f);
    dl->AddRect(
        ImVec2(c.x - hs, c.y - hs),
        ImVec2(c.x + hs, c.y + hs),
        cubeCol,
        1.0f,
        0,
        local ? t + 0.4f : t);
}

void DrawSnapIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // 3x3 grid (snap-to-grid)
    const float e = s * 0.85f;
    const float t = std::max(1.4f, s * 0.14f);
    const ImVec2 tl(c.x - e, c.y - e);
    const ImVec2 br(c.x + e, c.y + e);
    dl->AddRect(tl, br, col, 0.f, 0, t);

    const float x1 = c.x - e * 0.33f;
    const float x2 = c.x + e * 0.33f;
    const float y1 = c.y - e * 0.33f;
    const float y2 = c.y + e * 0.33f;
    dl->AddLine(ImVec2(x1, tl.y), ImVec2(x1, br.y), col, t);
    dl->AddLine(ImVec2(x2, tl.y), ImVec2(x2, br.y), col, t);
    dl->AddLine(ImVec2(tl.x, y1), ImVec2(br.x, y1), col, t);
    dl->AddLine(ImVec2(tl.x, y2), ImVec2(br.x, y2), col, t);
}

enum class ToolIcon : int
{
    Select = 0,
    Move,
    Rotate,
    Scale,
    Space,
    Snap,
};

void DrawToolIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, ToolIcon kind, ImU32 fg, bool spaceLocal)
{
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float s = std::min(max.x - min.x, max.y - min.y) * 0.34f;

    switch (kind)
    {
    case ToolIcon::Select: DrawSelectIcon(dl, c, s, fg); break;
    case ToolIcon::Move:   DrawMoveIcon(dl, c, s, fg); break;
    case ToolIcon::Rotate: DrawRotateIcon(dl, c, s, fg); break;
    case ToolIcon::Scale:  DrawScaleIcon(dl, c, s, fg); break;
    case ToolIcon::Space:  DrawSpaceIcon(dl, c, s, fg, spaceLocal); break;
    case ToolIcon::Snap:   DrawSnapIcon(dl, c, s, fg); break;
    }
}

bool ToolButton(const char* id, ToolIcon icon, bool selected, const char* tip, bool spaceLocal = false)
{
    const ImVec2 size(kBtn, kBtn);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    // PressedOnClick: switch tool on mouse-down (more reliable over overlapping viewport canvas).
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_PressedOnClick);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const EditorColors& ec = GetEditorColors();

    // Reference style: subtle raised chip when selected, faint hover otherwise.
    if (selected)
        dl->AddRectFilled(p0, p1, IM_COL32(55, 58, 66, 255), kRound);
    else if (hovered)
        dl->AddRectFilled(p0, p1, IM_COL32(40, 42, 48, 220), kRound);

    if (selected)
        dl->AddRect(p0, p1, IM_COL32(120, 170, 255, 200), kRound, 0, 1.2f);
    else if (hovered)
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), kRound, 0, 1.0f);

    const ImU32 fg = selected ? IM_COL32(245, 247, 250, 255)
                              : (hovered ? IM_COL32(220, 224, 230, 255) : Col(ec.Text));
    DrawToolIcon(dl, p0, p1, icon, fg, spaceLocal);

    if (tip && hovered)
        ImGui::SetTooltip("%s", tip);

    return pressed;
}

void ToolbarSeparator(ImDrawList* dl, float height)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + 1.5f;
    dl->AddLine(ImVec2(x, p.y + 5.f), ImVec2(x, p.y + height - 5.f), IM_COL32(255, 255, 255, 32), 1.0f);
    ImGui::Dummy(ImVec2(5.f, height));
}

} // namespace

void BuildTransformGizmoToolbar(EditorUIState& editorUI)
{
    const auto operation = static_cast<ImGuizmo::OPERATION>(editorUI.GizmoOperation);
    const auto mode = static_cast<ImGuizmo::MODE>(editorUI.GizmoMode);
    const bool selectMode = !editorUI.GizmoEnabled;
    const bool isMove = !selectMode && (operation == ImGuizmo::TRANSLATE);
    const bool isRotate = !selectMode && (operation == ImGuizmo::ROTATE);
    const bool isScale = !selectMode && (operation == ImGuizmo::SCALE);
    const bool isLocal = (mode == ImGuizmo::LOCAL);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Select | Move | Rotate | Scale | Space | Snap  (matches reference strip)
    const int toolCount = 6;
    const float stripW = kPad * 2.f + kBtn * toolCount + kGap * (toolCount - 1) + 5.f * 2.f;
    const float stripH = kBtn + kPad * 2.f;

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

    if (ToolButton("##GizmoSelect", ToolIcon::Select, selectMode, "Select (Q)"))
        editorUI.GizmoEnabled = false;
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoMove", ToolIcon::Move, isMove, "Move (W)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoRotate", ToolIcon::Rotate, isRotate, "Rotate (E)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoScale", ToolIcon::Scale, isScale, "Scale (T)"))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);
    }

    ImGui::SameLine(0.f, 0.f);
    ToolbarSeparator(dl, kBtn);
    ImGui::SameLine(0.f, 0.f);

    if (ToolButton(
            "##GizmoSpace",
            ToolIcon::Space,
            false,
            isLocal ? "Local space (click for World)" : "World space (click for Local)",
            isLocal))
    {
        editorUI.GizmoMode = static_cast<int>(
            isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL);
    }
    ImGui::SameLine(0.f, kGap);

    if (ToolButton("##GizmoSnap", ToolIcon::Snap, editorUI.GizmoSnapEnabled, "Snap to grid"))
        editorUI.GizmoSnapEnabled = !editorUI.GizmoSnapEnabled;

    ImGui::EndGroup();
}

} // namespace caustica::editor
