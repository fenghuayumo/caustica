#include "common/EditorTheme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace caustica::editor
{
namespace
{

ImVec4 Rgba(float r, float g, float b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

ImVec4 WithAlpha(const ImVec4& c, float a)
{
    return ImVec4(c.x, c.y, c.z, a);
}

struct ThemePalette
{
    const char* name;
    EditorColors colors;
    ImVec4 bg0;
    ImVec4 bg1;
    ImVec4 bg2;
    ImVec4 bg3;
    ImVec4 border;
    ImVec4 separator;
    ImVec4 textDisabled;
    ImVec4 frameHovered;
    ImVec4 scrollbarGrab;
    ImVec4 scrollbarGrabHovered;
};

// Dark — near-black default (Blender / VS Code Dark feel).
const ThemePalette kDark = {
    "Dark",
    {
        Rgba(230, 230, 230), Rgba(140, 140, 140), Rgba(232, 168, 96), Rgba(120, 188, 168),
        Rgba(64, 156, 196), Rgba(92, 180, 216), Rgba(48, 128, 168),
        Rgba(28, 28, 28, 0.92f), Rgba(42, 42, 42, 0.96f), Rgba(64, 156, 196, 0.50f),
    },
    Rgba(18, 18, 18), Rgba(24, 24, 24), Rgba(32, 32, 32), Rgba(40, 40, 40),
    Rgba(56, 56, 56), Rgba(40, 40, 40), Rgba(110, 110, 110),
    Rgba(48, 48, 48), Rgba(64, 64, 64, 0.85f), Rgba(96, 96, 96, 0.95f),
};

// Graphite — cool mid-gray shell (previous default).
const ThemePalette kGraphite = {
    "Graphite",
    {
        Rgba(236, 238, 241), Rgba(148, 154, 162), Rgba(236, 176, 112), Rgba(120, 196, 176),
        Rgba(72, 168, 184), Rgba(98, 192, 206), Rgba(52, 140, 156),
        Rgba(38, 42, 48, 0.88f), Rgba(54, 60, 68, 0.95f), Rgba(72, 168, 184, 0.50f),
    },
    Rgba(24, 26, 29), Rgba(30, 33, 37), Rgba(38, 42, 47), Rgba(46, 51, 57),
    Rgba(58, 64, 72), Rgba(44, 48, 54), Rgba(118, 124, 132),
    Rgba(52, 58, 66), Rgba(72, 80, 90, 0.80f), Rgba(96, 106, 118, 0.95f),
};

// Midnight — deep blue-black with sky accent.
const ThemePalette kMidnight = {
    "Midnight",
    {
        Rgba(220, 228, 240), Rgba(130, 142, 160), Rgba(240, 176, 110), Rgba(120, 180, 200),
        Rgba(80, 148, 230), Rgba(110, 172, 240), Rgba(56, 116, 196),
        Rgba(22, 28, 40, 0.92f), Rgba(34, 44, 62, 0.96f), Rgba(80, 148, 230, 0.50f),
    },
    Rgba(14, 18, 28), Rgba(20, 26, 38), Rgba(28, 36, 52), Rgba(36, 46, 64),
    Rgba(52, 64, 88), Rgba(32, 40, 56), Rgba(100, 112, 132),
    Rgba(42, 54, 74), Rgba(60, 76, 104, 0.85f), Rgba(90, 112, 148, 0.95f),
};

// Nord — soft arctic dark (Nord-inspired).
const ThemePalette kNord = {
    "Nord",
    {
        Rgba(216, 222, 233), Rgba(129, 161, 193), Rgba(235, 203, 139), Rgba(143, 188, 187),
        Rgba(136, 192, 208), Rgba(162, 210, 222), Rgba(94, 160, 178),
        Rgba(46, 52, 64, 0.92f), Rgba(59, 66, 82, 0.96f), Rgba(136, 192, 208, 0.50f),
    },
    Rgba(36, 41, 51), Rgba(46, 52, 64), Rgba(59, 66, 82), Rgba(67, 76, 94),
    Rgba(76, 86, 106), Rgba(59, 66, 82), Rgba(112, 124, 140),
    Rgba(67, 76, 94), Rgba(76, 86, 106, 0.85f), Rgba(94, 110, 132, 0.95f),
};

// Warm — charcoal with amber accent.
const ThemePalette kWarm = {
    "Warm",
    {
        Rgba(236, 230, 220), Rgba(160, 148, 132), Rgba(240, 180, 96), Rgba(196, 168, 120),
        Rgba(220, 152, 72), Rgba(236, 176, 96), Rgba(184, 120, 48),
        Rgba(36, 30, 26, 0.92f), Rgba(52, 44, 38, 0.96f), Rgba(220, 152, 72, 0.50f),
    },
    Rgba(22, 18, 16), Rgba(30, 26, 22), Rgba(40, 34, 28), Rgba(50, 42, 34),
    Rgba(68, 56, 46), Rgba(44, 36, 30), Rgba(120, 108, 96),
    Rgba(58, 48, 40), Rgba(72, 60, 50, 0.85f), Rgba(100, 84, 68, 0.95f),
};

// Slate — cool blue-gray, closer to Unity / professional DCC.
const ThemePalette kSlate = {
    "Slate",
    {
        Rgba(228, 232, 236), Rgba(140, 150, 160), Rgba(230, 170, 100), Rgba(120, 180, 170),
        Rgba(90, 170, 150), Rgba(118, 196, 176), Rgba(64, 140, 124),
        Rgba(40, 44, 48, 0.92f), Rgba(54, 60, 66, 0.96f), Rgba(90, 170, 150, 0.50f),
    },
    Rgba(28, 30, 34), Rgba(36, 40, 44), Rgba(46, 50, 56), Rgba(56, 62, 68),
    Rgba(70, 78, 86), Rgba(50, 54, 60), Rgba(112, 120, 128),
    Rgba(60, 66, 74), Rgba(74, 82, 90, 0.85f), Rgba(100, 110, 120, 0.95f),
};

const ThemePalette* const kThemes[] = {
    &kDark,
    &kGraphite,
    &kMidnight,
    &kNord,
    &kWarm,
    &kSlate,
};
static_assert(sizeof(kThemes) / sizeof(kThemes[0]) == static_cast<int>(EditorThemeId::Count));

EditorThemeId g_themeId = EditorThemeId::Dark;

const ThemePalette& ActivePalette()
{
    const int index = std::clamp(static_cast<int>(g_themeId), 0, static_cast<int>(EditorThemeId::Count) - 1);
    return *kThemes[index];
}

float Scaled(float value, float displayScale)
{
    const float s = (displayScale > 0.0f) ? displayScale : 1.0f;
    return static_cast<float>(static_cast<int>(value * s));
}

ImVec2 Scaled(ImVec2 value, float displayScale)
{
    return ImVec2(Scaled(value.x, displayScale), Scaled(value.y, displayScale));
}

std::filesystem::path ResolvePreferencesPath()
{
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return std::filesystem::path(appData) / "Caustica" / "preferences.ini";
#endif
    return std::filesystem::current_path() / "caustica_preferences.ini";
}

EditorThemeId ThemeIdFromName(const std::string& name)
{
    for (int i = 0; i < static_cast<int>(EditorThemeId::Count); ++i)
    {
        if (name == kThemes[i]->name)
            return static_cast<EditorThemeId>(i);
    }
    return EditorThemeId::Dark;
}

void ApplyPalette(const ThemePalette& pal, float displayScale)
{
    ImGuiStyle& style = ImGui::GetStyle();

    // ImGui 1.92 keeps live font metrics on the style. Never assign a fresh
    // ImGuiStyle() here — that zeroes FontSizeBase / FontScale* and can AV on
    // the next text draw if DPI notify re-applies the theme after first frame.
    const float fontSizeBase = style.FontSizeBase;
    const float fontScaleMain = style.FontScaleMain;
    const float fontScaleDpi = style.FontScaleDpi;
    const float nextFrameFontSizeBase = style._NextFrameFontSizeBase;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = Scaled(3.0f, displayScale);
    style.PopupRounding = Scaled(4.0f, displayScale);
    style.ScrollbarRounding = Scaled(3.0f, displayScale);
    style.GrabRounding = Scaled(3.0f, displayScale);
    style.TabRounding = 0.0f;

    style.WindowPadding = Scaled(ImVec2(10.0f, 8.0f), displayScale);
    style.FramePadding = Scaled(ImVec2(8.0f, 4.0f), displayScale);
    style.CellPadding = Scaled(ImVec2(6.0f, 3.0f), displayScale);
    style.ItemSpacing = Scaled(ImVec2(8.0f, 5.0f), displayScale);
    style.ItemInnerSpacing = Scaled(ImVec2(5.0f, 4.0f), displayScale);
    style.IndentSpacing = Scaled(16.0f, displayScale);
    style.ScrollbarSize = Scaled(11.0f, displayScale);
    style.GrabMinSize = Scaled(9.0f, displayScale);

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = Scaled(2.0f, displayScale);
    style.DockingSeparatorSize = Scaled(1.0f, displayScale);

    style.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.TabCloseButtonMinWidthSelected = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;

    const EditorColors& col = pal.colors;
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = col.Text;
    c[ImGuiCol_TextDisabled] = pal.textDisabled;
    c[ImGuiCol_WindowBg] = WithAlpha(pal.bg0, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = WithAlpha(pal.bg2, 0.98f);
    c[ImGuiCol_Border] = WithAlpha(pal.border, 0.55f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = WithAlpha(pal.bg2, 1.0f);
    c[ImGuiCol_FrameBgHovered] = WithAlpha(pal.frameHovered, 1.0f);
    c[ImGuiCol_FrameBgActive] = WithAlpha(col.Accent, 0.28f);

    c[ImGuiCol_TitleBg] = WithAlpha(pal.bg1, 1.0f);
    c[ImGuiCol_TitleBgActive] = WithAlpha(pal.bg1, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = WithAlpha(pal.bg1, 1.0f);

    c[ImGuiCol_MenuBarBg] = WithAlpha(pal.bg1, 1.0f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = pal.scrollbarGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = pal.scrollbarGrabHovered;
    c[ImGuiCol_ScrollbarGrabActive] = WithAlpha(col.Accent, 0.80f);

    c[ImGuiCol_CheckMark] = col.AccentHovered;
    c[ImGuiCol_SliderGrab] = WithAlpha(col.Accent, 0.85f);
    c[ImGuiCol_SliderGrabActive] = col.AccentHovered;

    c[ImGuiCol_Button] = WithAlpha(pal.bg3, 1.0f);
    c[ImGuiCol_ButtonHovered] = WithAlpha(col.Accent, 0.40f);
    c[ImGuiCol_ButtonActive] = WithAlpha(col.Accent, 0.65f);

    c[ImGuiCol_Header] = WithAlpha(pal.bg3, 0.85f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(col.Accent, 0.26f);
    c[ImGuiCol_HeaderActive] = WithAlpha(col.Accent, 0.40f);

    c[ImGuiCol_Separator] = WithAlpha(pal.separator, 1.0f);
    c[ImGuiCol_SeparatorHovered] = WithAlpha(col.Accent, 0.55f);
    c[ImGuiCol_SeparatorActive] = col.Accent;

    c[ImGuiCol_ResizeGrip] = WithAlpha(col.Accent, 0.10f);
    c[ImGuiCol_ResizeGripHovered] = WithAlpha(col.Accent, 0.40f);
    c[ImGuiCol_ResizeGripActive] = WithAlpha(col.Accent, 0.65f);

    c[ImGuiCol_Tab] = WithAlpha(pal.bg1, 1.0f);
    c[ImGuiCol_TabHovered] = WithAlpha(pal.bg3, 1.0f);
    c[ImGuiCol_TabSelected] = WithAlpha(pal.bg0, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = col.Accent;
    c[ImGuiCol_TabDimmed] = WithAlpha(pal.bg1, 1.0f);
    c[ImGuiCol_TabDimmedSelected] = WithAlpha(pal.bg0, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(col.Accent, 0.75f);

    c[ImGuiCol_DockingPreview] = WithAlpha(col.Accent, 0.32f);
    c[ImGuiCol_DockingEmptyBg] = WithAlpha(pal.bg0, 1.0f);

    c[ImGuiCol_PlotLines] = col.Accent;
    c[ImGuiCol_PlotLinesHovered] = col.AccentHovered;
    c[ImGuiCol_PlotHistogram] = col.Accent;
    c[ImGuiCol_PlotHistogramHovered] = col.AccentHovered;

    c[ImGuiCol_TableHeaderBg] = WithAlpha(pal.bg3, 1.0f);
    c[ImGuiCol_TableBorderStrong] = WithAlpha(pal.border, 0.70f);
    c[ImGuiCol_TableBorderLight] = WithAlpha(pal.border, 0.35f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Rgba(255, 255, 255, 0.02f);

    c[ImGuiCol_TextLink] = col.AccentHovered;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(col.Accent, 0.35f);
    c[ImGuiCol_InputTextCursor] = col.Text;
    c[ImGuiCol_TreeLines] = WithAlpha(pal.border, 0.65f);

    c[ImGuiCol_DragDropTarget] = WithAlpha(col.AccentHovered, 0.90f);
    c[ImGuiCol_NavCursor] = col.Accent;
    c[ImGuiCol_NavWindowingHighlight] = Rgba(255, 255, 255, 0.55f);
    c[ImGuiCol_NavWindowingDimBg] = Rgba(0, 0, 0, 0.50f);
    c[ImGuiCol_ModalWindowDimBg] = Rgba(0, 0, 0, 0.60f);

    style.FontSizeBase = fontSizeBase;
    style.FontScaleMain = fontScaleMain;
    style.FontScaleDpi = fontScaleDpi;
    style._NextFrameFontSizeBase = nextFrameFontSizeBase;
    style._MainScale = (displayScale > 0.0f) ? displayScale : 1.0f;
}

} // namespace

const EditorColors& GetEditorColors()
{
    return ActivePalette().colors;
}

EditorThemeId GetEditorThemeId()
{
    return g_themeId;
}

void SetEditorThemeId(EditorThemeId id)
{
    g_themeId = static_cast<EditorThemeId>(
        std::clamp(static_cast<int>(id), 0, static_cast<int>(EditorThemeId::Count) - 1));
}

const char* GetEditorThemeName(EditorThemeId id)
{
    const int index = std::clamp(static_cast<int>(id), 0, static_cast<int>(EditorThemeId::Count) - 1);
    return kThemes[index]->name;
}

int GetEditorThemeCount()
{
    return static_cast<int>(EditorThemeId::Count);
}

void LoadEditorThemePreference()
{
    const auto path = ResolvePreferencesPath();
    std::ifstream in(path);
    if (!in)
        return;

    std::string line;
    while (std::getline(in, line))
    {
        if (line.rfind("Theme=", 0) == 0)
        {
            SetEditorThemeId(ThemeIdFromName(line.substr(6)));
            break;
        }
    }
}

void SaveEditorThemePreference()
{
    const auto path = ResolvePreferencesPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return;
    out << "Theme=" << GetEditorThemeName(GetEditorThemeId()) << '\n';
}

void ApplyEditorTheme(float displayScale)
{
    ApplyPalette(ActivePalette(), displayScale);
}

void ApplyEditorTheme(EditorThemeId id, float displayScale)
{
    SetEditorThemeId(id);
    ApplyPalette(ActivePalette(), displayScale);
}

void PushToolbarButtonColors(bool selected)
{
    const EditorColors& col = GetEditorColors();
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, col.Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col.AccentHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col.AccentActive);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, col.ToolbarIdle);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col.ToolbarIdleHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col.ToolbarIdleActive);
    }
}

void PopToolbarButtonColors()
{
    ImGui::PopStyleColor(3);
}

} // namespace caustica::editor
