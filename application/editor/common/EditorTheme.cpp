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

// Deep dark shell (near-black) + cool accent — closer to Blender / VS Code Dark
// than mid-gray graphite.
const EditorColors kColors = {
    /* Text */               Rgba(230, 230, 230),
    /* TextMuted */          Rgba(140, 140, 140),
    /* TextWarning */        Rgba(232, 168, 96),
    /* TextCategory */       Rgba(120, 188, 168),

    /* Accent */             Rgba(64, 156, 196),
    /* AccentHovered */      Rgba(92, 180, 216),
    /* AccentActive */       Rgba(48, 128, 168),

    /* ToolbarIdle */        Rgba(28, 28, 28, 0.92f),
    /* ToolbarIdleHovered */ Rgba(42, 42, 42, 0.96f),
    /* ToolbarIdleActive */  Rgba(64, 156, 196, 0.50f),
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

    // Flat dark DCC shell.
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

    // Near-black stack: window / chrome / controls / raised.
    const ImVec4 bg0 = Rgba(18, 18, 18);
    const ImVec4 bg1 = Rgba(24, 24, 24);
    const ImVec4 bg2 = Rgba(32, 32, 32);
    const ImVec4 bg3 = Rgba(40, 40, 40);
    const ImVec4 border = Rgba(56, 56, 56);
    const ImVec4 separator = Rgba(40, 40, 40, 1.0f);

    const EditorColors& col = kColors;
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = col.Text;
    c[ImGuiCol_TextDisabled] = Rgba(110, 110, 110);
    c[ImGuiCol_WindowBg] = WithAlpha(bg0, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = WithAlpha(bg2, 0.98f);
    c[ImGuiCol_Border] = WithAlpha(border, 0.55f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = WithAlpha(bg2, 1.0f);
    c[ImGuiCol_FrameBgHovered] = Rgba(48, 48, 48, 1.0f);
    c[ImGuiCol_FrameBgActive] = WithAlpha(col.Accent, 0.28f);

    c[ImGuiCol_TitleBg] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TitleBgActive] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = WithAlpha(bg1, 1.0f);

    c[ImGuiCol_MenuBarBg] = WithAlpha(bg1, 1.0f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgba(64, 64, 64, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgba(96, 96, 96, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive] = WithAlpha(col.Accent, 0.80f);

    c[ImGuiCol_CheckMark] = col.AccentHovered;
    c[ImGuiCol_SliderGrab] = WithAlpha(col.Accent, 0.85f);
    c[ImGuiCol_SliderGrabActive] = col.AccentHovered;

    c[ImGuiCol_Button] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_ButtonHovered] = WithAlpha(col.Accent, 0.40f);
    c[ImGuiCol_ButtonActive] = WithAlpha(col.Accent, 0.65f);

    c[ImGuiCol_Header] = WithAlpha(bg3, 0.85f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(col.Accent, 0.26f);
    c[ImGuiCol_HeaderActive] = WithAlpha(col.Accent, 0.40f);

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
    c[ImGuiCol_TableRowBgAlt] = Rgba(255, 255, 255, 0.02f);

    c[ImGuiCol_TextLink] = col.AccentHovered;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(col.Accent, 0.35f);
    c[ImGuiCol_InputTextCursor] = col.Text;
    c[ImGuiCol_TreeLines] = WithAlpha(border, 0.65f);

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
