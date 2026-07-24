#pragma once

#include <imgui.h>

namespace caustica::editor
{

// Semantic colors used by editor panels (section labels, warnings, toolbars).
// Source of truth for palette tokens; ImGui style slots are filled by ApplyEditorTheme().
struct EditorColors
{
    ImVec4 Text;
    ImVec4 TextMuted;
    ImVec4 TextWarning;
    ImVec4 TextCategory;

    ImVec4 Accent;
    ImVec4 AccentHovered;
    ImVec4 AccentActive;

    ImVec4 ToolbarIdle;
    ImVec4 ToolbarIdleHovered;
    ImVec4 ToolbarIdleActive;
};

const EditorColors& GetEditorColors();

// Apply the Caustica editor Dark theme (near-black) to the current ImGui context.
// displayScale should match the OS/UI scale used by ImGui_Renderer (typically 1.0 at init).
void ApplyEditorTheme(float displayScale = 1.0f);

// Shared toolbar toggle button styling (selected = accent, idle = muted).
void PushToolbarButtonColors(bool selected);
void PopToolbarButtonColors();

} // namespace caustica::editor
