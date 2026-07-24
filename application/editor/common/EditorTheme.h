#pragma once

#include <imgui.h>

namespace caustica::editor
{

enum class EditorThemeId : int
{
    Dark = 0,
    Graphite,
    Midnight,
    Nord,
    Warm,
    Slate,
    Count
};

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

EditorThemeId GetEditorThemeId();
void SetEditorThemeId(EditorThemeId id);
const char* GetEditorThemeName(EditorThemeId id);
int GetEditorThemeCount();

// Load / save theme preference next to editor.ini (%APPDATA%\Caustica\preferences.ini).
void LoadEditorThemePreference();
void SaveEditorThemePreference();

// Apply the active (or specified) editor theme to the current ImGui context.
// displayScale should match the OS/UI scale used by ImGui_Renderer (typically 1.0 at init).
void ApplyEditorTheme(float displayScale = 1.0f);
void ApplyEditorTheme(EditorThemeId id, float displayScale);

// Shared toolbar toggle button styling (selected = accent, idle = muted).
void PushToolbarButtonColors(bool selected);
void PopToolbarButtonColors();

} // namespace caustica::editor
