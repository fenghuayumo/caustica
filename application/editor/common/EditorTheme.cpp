#include "common/EditorTheme.h"

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

// Cool graphite shell + clear cyan accent (readable with proportional UI fonts).
const EditorColors kColors = {
    /* Text */               Rgba(236, 238, 241),
    /* TextMuted */          Rgba(148, 154, 162),
    /* TextWarning */        Rgba(236, 176, 112),
    /* TextCategory */       Rgba(120, 196, 176),

    /* Accent */             Rgba(72, 168, 184),
    /* AccentHovered */      Rgba(98, 192, 206),
    /* AccentActive */       Rgba(52, 140, 156),

    /* ToolbarIdle */        Rgba(38, 42, 48, 0.88f),
    /* ToolbarIdleHovered */ Rgba(54, 60, 68, 0.95f),
    /* ToolbarIdleActive */  Rgba(72, 168, 184, 0.50f),
};

float Scaled(float value, float displayScale)
{
    const float s = (displayScale > 0.0f) ? displayScale : 1.0f;
    return static_cast<float>(static_cast<int>(value * s));
}

ImVec2 Scaled(ImVec2 value, float displayScale)
{
    return ImVec2(Scaled(value.x, displayScale), Scaled(value.y, displayScale));
}

} // namespace

const EditorColors& GetEditorColors()
{
    return kColors;
}

void ApplyEditorTheme(float displayScale)
{
    ImGuiStyle& style = ImGui::GetStyle();

    // ImGui 1.92 keeps live font metrics on the style. Never assign a fresh
    // ImGuiStyle() here — that zeroes FontSizeBase / FontScale* and can AV on
    // the next text draw if DPI notify re-applies the theme after first frame.
    const float fontSizeBase = style.FontSizeBase;
    const float fontScaleMain = style.FontScaleMain;
    const float fontScaleDpi = style.FontScaleDpi;
    const float nextFrameFontSizeBase = style._NextFrameFontSizeBase;

    // Flat DCC shell tuned for proportional UI type.
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

    const ImVec4 bg0 = Rgba(24, 26, 29);
    const ImVec4 bg1 = Rgba(30, 33, 37);
    const ImVec4 bg2 = Rgba(38, 42, 47);
    const ImVec4 bg3 = Rgba(46, 51, 57);
    const ImVec4 border = Rgba(58, 64, 72);
    const ImVec4 separator = Rgba(44, 48, 54, 1.0f);

    const EditorColors& col = kColors;
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = col.Text;
    c[ImGuiCol_TextDisabled] = Rgba(118, 124, 132);
    c[ImGuiCol_WindowBg] = WithAlpha(bg0, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = WithAlpha(bg2, 0.98f);
    c[ImGuiCol_Border] = WithAlpha(border, 0.45f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = WithAlpha(bg2, 1.0f);
    c[ImGuiCol_FrameBgHovered] = Rgba(52, 58, 66, 1.0f);
    c[ImGuiCol_FrameBgActive] = WithAlpha(col.Accent, 0.30f);

    c[ImGuiCol_TitleBg] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TitleBgActive] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = WithAlpha(bg1, 1.0f);

    c[ImGuiCol_MenuBarBg] = WithAlpha(bg1, 1.0f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgba(72, 80, 90, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgba(96, 106, 118, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive] = WithAlpha(col.Accent, 0.80f);

    c[ImGuiCol_CheckMark] = col.AccentHovered;
    c[ImGuiCol_SliderGrab] = WithAlpha(col.Accent, 0.85f);
    c[ImGuiCol_SliderGrabActive] = col.AccentHovered;

    c[ImGuiCol_Button] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_ButtonHovered] = WithAlpha(col.Accent, 0.42f);
    c[ImGuiCol_ButtonActive] = WithAlpha(col.Accent, 0.68f);

    c[ImGuiCol_Header] = WithAlpha(bg3, 0.90f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(col.Accent, 0.28f);
    c[ImGuiCol_HeaderActive] = WithAlpha(col.Accent, 0.42f);

    c[ImGuiCol_Separator] = separator;
    c[ImGuiCol_SeparatorHovered] = WithAlpha(col.Accent, 0.55f);
    c[ImGuiCol_SeparatorActive] = col.Accent;

    c[ImGuiCol_ResizeGrip] = WithAlpha(col.Accent, 0.10f);
    c[ImGuiCol_ResizeGripHovered] = WithAlpha(col.Accent, 0.40f);
    c[ImGuiCol_ResizeGripActive] = WithAlpha(col.Accent, 0.65f);

    c[ImGuiCol_Tab] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TabHovered] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_TabSelected] = WithAlpha(bg0, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = col.Accent;
    c[ImGuiCol_TabDimmed] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TabDimmedSelected] = WithAlpha(bg0, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(col.Accent, 0.75f);

    c[ImGuiCol_DockingPreview] = WithAlpha(col.Accent, 0.32f);
    c[ImGuiCol_DockingEmptyBg] = WithAlpha(bg0, 1.0f);

    c[ImGuiCol_PlotLines] = col.Accent;
    c[ImGuiCol_PlotLinesHovered] = col.AccentHovered;
    c[ImGuiCol_PlotHistogram] = col.Accent;
    c[ImGuiCol_PlotHistogramHovered] = col.AccentHovered;

    c[ImGuiCol_TableHeaderBg] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_TableBorderStrong] = WithAlpha(border, 0.70f);
    c[ImGuiCol_TableBorderLight] = WithAlpha(border, 0.35f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Rgba(255, 255, 255, 0.025f);

    c[ImGuiCol_TextLink] = col.AccentHovered;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(col.Accent, 0.35f);
    c[ImGuiCol_InputTextCursor] = col.Text;
    c[ImGuiCol_TreeLines] = WithAlpha(border, 0.65f);

    c[ImGuiCol_DragDropTarget] = WithAlpha(col.AccentHovered, 0.90f);
    c[ImGuiCol_NavCursor] = col.Accent;
    c[ImGuiCol_NavWindowingHighlight] = Rgba(255, 255, 255, 0.55f);
    c[ImGuiCol_NavWindowingDimBg] = Rgba(0, 0, 0, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = Rgba(0, 0, 0, 0.55f);

    style.FontSizeBase = fontSizeBase;
    style.FontScaleMain = fontScaleMain;
    style.FontScaleDpi = fontScaleDpi;
    style._NextFrameFontSizeBase = nextFrameFontSizeBase;
    style._MainScale = (displayScale > 0.0f) ? displayScale : 1.0f;
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
