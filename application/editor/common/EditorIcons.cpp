#include "common/EditorIcons.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>

namespace caustica::editor
{

IconUtf8::IconUtf8(ImWchar codepoint)
{
    ImTextCharToUtf8(bytes, codepoint);
}

void DrawIconGlyph(ImDrawList* dl, ImVec2 min, ImVec2 max, ImWchar codepoint, ImU32 col)
{
    ImFont* font = ImGui::GetFont();
    ImFontBaked* baked = font ? font->GetFontBaked(ImGui::GetFontSize()) : nullptr;
    if (!font || !baked || !baked->FindGlyphNoFallback(codepoint))
    {
        // Font not merged / glyph missing — quiet placeholder.
        const float s = std::min(max.x - min.x, max.y - min.y) * 0.28f;
        const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 1.5f, 0, 1.2f);
        return;
    }

    const IconUtf8 utf8(codepoint);
    const float size = std::min(max.x - min.x, max.y - min.y) * 0.62f;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, utf8.c_str());
    const ImVec2 pos(
        (min.x + max.x - ts.x) * 0.5f,
        (min.y + max.y - ts.y) * 0.5f);
    dl->AddText(font, size, pos, col, utf8.c_str());
}

void DrawHierarchyTypeIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, HierarchyTypeIcon kind, ImU32 col)
{
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float s = std::min(w, h);
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float t = std::max(1.25f, s * 0.09f);

    switch (kind)
    {
    case HierarchyTypeIcon::Mesh:
    {
        // Clean isometric cube (Lucide / DCC style).
        const float hw = s * 0.34f;
        const float hh = s * 0.20f;
        const float vd = s * 0.28f;
        const ImVec2 top(c.x, c.y - vd);
        const ImVec2 left(c.x - hw, c.y - hh * 0.15f);
        const ImVec2 right(c.x + hw, c.y - hh * 0.15f);
        const ImVec2 bot(c.x, c.y + vd * 0.55f);
        const ImVec2 leftBot(c.x - hw, c.y + vd * 0.35f);
        const ImVec2 rightBot(c.x + hw, c.y + vd * 0.35f);

        dl->AddLine(top, left, col, t);
        dl->AddLine(top, right, col, t);
        dl->AddLine(left, bot, col, t);
        dl->AddLine(right, bot, col, t);
        dl->AddLine(left, leftBot, col, t);
        dl->AddLine(right, rightBot, col, t);
        dl->AddLine(bot, leftBot, col, t);
        dl->AddLine(bot, rightBot, col, t);
        break;
    }
    case HierarchyTypeIcon::GaussianSplat:
    {
        // Three soft disks — reads as a splat / point cloud at small size.
        const float r0 = s * 0.16f;
        const float r1 = s * 0.13f;
        const float r2 = s * 0.11f;
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.18f), r0, col, 12);
        dl->AddCircleFilled(ImVec2(c.x - s * 0.22f, c.y + s * 0.16f), r1, col, 12);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.24f, c.y + s * 0.14f), r2, col, 12);
        break;
    }
    case HierarchyTypeIcon::Group:
    default:
    {
        // Folder tab + body.
        const float left = c.x - s * 0.36f;
        const float right = c.x + s * 0.36f;
        const float top = c.y - s * 0.18f;
        const float bot = c.y + s * 0.28f;
        const float tabR = c.x - s * 0.02f;
        const float tabT = c.y - s * 0.34f;
        dl->AddRectFilled(ImVec2(left, top), ImVec2(tabR, top + s * 0.12f), col, s * 0.06f);
        dl->AddRect(ImVec2(left, top), ImVec2(right, bot), col, s * 0.08f, 0, t);
        (void)tabT;
        break;
    }
    }
}

bool IconButton(const char* id, ImWchar codepoint, bool active, const char* tip)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size, p0.y + size);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();

    ImU32 col;
    if (active)
        col = ImGui::GetColorU32(ImGuiCol_Text);
    else if (hovered)
        col = ImGui::GetColorU32(ImGuiCol_Text);
    else
        col = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    DrawIconGlyph(ImGui::GetWindowDrawList(), p0, p1, codepoint, col);

    if (tip && hovered)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

} // namespace caustica::editor
