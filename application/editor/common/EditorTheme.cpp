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

const EditorColors kColors = {
    /* Text */               Rgba(232, 234, 237),
    /* TextMuted */          Rgba(154, 160, 166),
    /* TextWarning */        Rgba(232, 168, 120),
    /* TextCategory */       Rgba(126, 200, 175),

    /* Accent */             Rgba(61, 158, 173),
    /* AccentHovered */      Rgba(80, 180, 196),
    /* AccentActive */       Rgba(45, 130, 145),

    /* ToolbarIdle */        Rgba(42, 47, 54, 0.85f),
    /* ToolbarIdleHovered */ Rgba(58, 64, 72, 0.95f),
    /* ToolbarIdleActive */  Rgba(61, 158, 173, 0.55f),
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

    // Geometry — write absolute scaled values (do not call ScaleAllSizes).
    style.WindowRounding = Scaled(4.0f, displayScale);
    style.ChildRounding = Scaled(3.0f, displayScale);
    style.FrameRounding = Scaled(3.0f, displayScale);
    style.PopupRounding = Scaled(3.0f, displayScale);
    style.ScrollbarRounding = Scaled(3.0f, displayScale);
    style.GrabRounding = Scaled(2.0f, displayScale);
    style.TabRounding = Scaled(3.0f, displayScale);

    style.WindowPadding = Scaled(ImVec2(10.0f, 10.0f), displayScale);
    style.FramePadding = Scaled(ImVec2(8.0f, 4.0f), displayScale);
    style.CellPadding = Scaled(ImVec2(6.0f, 3.0f), displayScale);
    style.ItemSpacing = Scaled(ImVec2(8.0f, 6.0f), displayScale);
    style.ItemInnerSpacing = Scaled(ImVec2(6.0f, 4.0f), displayScale);
    style.IndentSpacing = Scaled(16.0f, displayScale);
    style.ScrollbarSize = Scaled(12.0f, displayScale);
    style.GrabMinSize = Scaled(10.0f, displayScale);

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    // Palette — cool neutral dark + teal accent
    const ImVec4 bg0 = Rgba(26, 29, 33);
    const ImVec4 bg1 = Rgba(34, 38, 43);
    const ImVec4 bg2 = Rgba(42, 47, 54);
    const ImVec4 bg3 = Rgba(48, 54, 62);
    const ImVec4 border = Rgba(58, 64, 72);
    const ImVec4 separator = Rgba(58, 64, 72, 0.85f);

    const EditorColors& col = kColors;
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = col.Text;
    c[ImGuiCol_TextDisabled] = Rgba(120, 126, 134);
    c[ImGuiCol_WindowBg] = WithAlpha(bg0, 0.96f);
    c[ImGuiCol_ChildBg] = WithAlpha(bg1, 0.0f);
    c[ImGuiCol_PopupBg] = WithAlpha(bg2, 0.98f);
    c[ImGuiCol_Border] = WithAlpha(border, 0.70f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = WithAlpha(bg2, 0.90f);
    c[ImGuiCol_FrameBgHovered] = Rgba(58, 64, 72, 0.95f);
    c[ImGuiCol_FrameBgActive] = WithAlpha(col.Accent, 0.35f);

    c[ImGuiCol_TitleBg] = WithAlpha(bg1, 1.0f);
    c[ImGuiCol_TitleBgActive] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = WithAlpha(bg1, 0.85f);

    c[ImGuiCol_MenuBarBg] = WithAlpha(bg1, 1.0f);

    c[ImGuiCol_ScrollbarBg] = WithAlpha(bg0, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = Rgba(72, 80, 90, 0.90f);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgba(96, 106, 118, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive] = WithAlpha(col.Accent, 0.85f);

    c[ImGuiCol_CheckMark] = col.Accent;
    c[ImGuiCol_SliderGrab] = WithAlpha(col.Accent, 0.85f);
    c[ImGuiCol_SliderGrabActive] = col.AccentHovered;

    c[ImGuiCol_Button] = WithAlpha(bg3, 0.90f);
    c[ImGuiCol_ButtonHovered] = WithAlpha(col.Accent, 0.55f);
    c[ImGuiCol_ButtonActive] = WithAlpha(col.Accent, 0.80f);

    c[ImGuiCol_Header] = WithAlpha(col.Accent, 0.28f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(col.Accent, 0.45f);
    c[ImGuiCol_HeaderActive] = WithAlpha(col.Accent, 0.60f);

    c[ImGuiCol_Separator] = separator;
    c[ImGuiCol_SeparatorHovered] = WithAlpha(col.Accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = col.Accent;

    c[ImGuiCol_ResizeGrip] = WithAlpha(col.Accent, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = WithAlpha(col.Accent, 0.50f);
    c[ImGuiCol_ResizeGripActive] = WithAlpha(col.Accent, 0.75f);

    c[ImGuiCol_Tab] = WithAlpha(bg2, 0.90f);
    c[ImGuiCol_TabHovered] = WithAlpha(col.Accent, 0.55f);
    c[ImGuiCol_TabSelected] = WithAlpha(col.Accent, 0.40f);
    c[ImGuiCol_TabSelectedOverline] = col.Accent;
    c[ImGuiCol_TabDimmed] = WithAlpha(bg1, 0.90f);
    c[ImGuiCol_TabDimmedSelected] = WithAlpha(col.Accent, 0.30f);
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(col.Accent, 0.60f);

    c[ImGuiCol_PlotLines] = col.Accent;
    c[ImGuiCol_PlotLinesHovered] = col.AccentHovered;
    c[ImGuiCol_PlotHistogram] = col.Accent;
    c[ImGuiCol_PlotHistogramHovered] = col.AccentHovered;

    c[ImGuiCol_TableHeaderBg] = WithAlpha(bg3, 1.0f);
    c[ImGuiCol_TableBorderStrong] = WithAlpha(border, 0.90f);
    c[ImGuiCol_TableBorderLight] = WithAlpha(border, 0.50f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Rgba(255, 255, 255, 0.03f);

    c[ImGuiCol_TextLink] = col.AccentHovered;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(col.Accent, 0.35f);
    c[ImGuiCol_InputTextCursor] = col.Text;
    c[ImGuiCol_TreeLines] = WithAlpha(border, 0.80f);

    c[ImGuiCol_DragDropTarget] = WithAlpha(col.AccentHovered, 0.90f);
    c[ImGuiCol_NavCursor] = col.Accent;
    c[ImGuiCol_NavWindowingHighlight] = Rgba(255, 255, 255, 0.55f);
    c[ImGuiCol_NavWindowingDimBg] = Rgba(0, 0, 0, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = Rgba(0, 0, 0, 0.55f);

    // Restore font metrics wiped only if a caller ever did a full style assign.
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
