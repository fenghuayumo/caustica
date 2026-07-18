#pragma once

#include <imgui.h>

namespace caustica::editor
{

// Segoe Fluent / MDL2 Private Use Area glyphs (loaded via ImGuiManager).
namespace Icon
{
inline constexpr ImWchar Refresh = 0xE72C;
inline constexpr ImWchar Lock = 0xE72E;
inline constexpr ImWchar Unlock = 0xE785;
inline constexpr ImWchar Eye = 0xE7B3;       // RedEye
inline constexpr ImWchar Hide = 0xED1A;
inline constexpr ImWchar Search = 0xE721;
}

enum class HierarchyTypeIcon : int
{
    Group = 0,
    Mesh,
    GaussianSplat,
};

// UTF-8 buffer for a single PUA glyph (ImGui text APIs).
struct IconUtf8
{
    char bytes[8]{};
    explicit IconUtf8(ImWchar codepoint);
    const char* c_str() const { return bytes; }
};

// Draw a Fluent glyph centered in [min,max] (falls back to a small square if font missing).
void DrawIconGlyph(ImDrawList* dl, ImVec2 min, ImVec2 max, ImWchar codepoint, ImU32 col);

// Hierarchy row type marks — stroke icons tuned for ~14–16px (Fluent Package looks wrong here).
void DrawHierarchyTypeIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, HierarchyTypeIcon kind, ImU32 col);

// InvisibleButton + centered Fluent glyph. Returns true when clicked.
bool IconButton(const char* id, ImWchar codepoint, bool active, const char* tip);

} // namespace caustica::editor
