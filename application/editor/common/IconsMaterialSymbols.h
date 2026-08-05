// Subset of Material Symbols Rounded macros used by the editor toolbars.
// Font data: External/material_symbols/material_symbols_rounded_regular.h
// Full set: https://github.com/juliettef/IconFontCppHeaders
#pragma once

#include <imgui.h>

#define ICON_MIN_MS 0xe003
#define ICON_MAX_MS 0xf8ff

#define ICON_MS_PLAY_ARROW            "\xee\x80\xb7" // U+e037
#define ICON_MS_PAUSE                 "\xee\x80\xb4" // U+e034
#define ICON_MS_SKIP_NEXT             "\xee\x81\x84" // U+e044
#define ICON_MS_SKIP_PREVIOUS         "\xee\x81\x85" // U+e045
#define ICON_MS_DELETE                "\xee\xa4\xae" // U+e92e
#define ICON_MS_ROTATE_90_DEGREES_CCW "\xee\x90\x98" // U+e418
#define ICON_MS_GRID_ON               "\xee\x8f\xac" // U+e3ec
#define ICON_MS_ZOOM_OUT_MAP          "\xee\x95\xab" // U+e56b
#define ICON_MS_FIRST_PAGE            "\xee\x97\x9c" // U+e5dc
#define ICON_MS_LAST_PAGE             "\xee\x97\x9d" // U+e5dd
#define ICON_MS_AUTORENEW             "\xee\xa1\xa3" // U+e863  circular arrows (rotate)
#define ICON_MS_PUBLIC                "\xee\xa0\x8b" // U+e80b
#define ICON_MS_OPEN_WITH             "\xee\xa2\x9f" // U+e89f  four-way arrows (translate)
#define ICON_MS_VISIBILITY            "\xee\xa3\xb4" // U+e8f4
#define ICON_MS_VISIBILITY_OFF        "\xee\xa3\xb5" // U+e8f5
#define ICON_MS_ATTRACTIONS           "\xee\xa9\x92" // U+ea52  magnet (snap)
#define ICON_MS_DIAMOND               "\xee\xab\x95" // U+ead5
#define ICON_MS_VIEW_IN_AR            "\xee\xbf\x89" // U+efc9
#define ICON_MS_ADD_DIAMOND           "\xef\x92\x9c" // U+f49c
#define ICON_MS_ARROW_SELECTOR_TOOL   "\xef\xa0\xaf" // U+f82f

// Console Log / Output Log toolbar
#define ICON_MS_DELETE_SWEEP          "\xee\x85\xac" // U+e16c  clear log
#define ICON_MS_CHAT                  "\xee\x83\x89" // U+e0c9  messages
#define ICON_MS_WARNING               "\xef\x82\x83" // U+f083
#define ICON_MS_ERROR                 "\xef\xa2\xb6" // U+f8b6
#define ICON_MS_WRAP_TEXT             "\xee\x89\x9b" // U+e25b
#define ICON_MS_VERTICAL_ALIGN_BOTTOM "\xee\x89\x98" // U+e258  scroll to end
#define ICON_MS_SEARCH                "\xef\xbd\xba" // U+ef7a
#define ICON_MS_INFO                  "\xee\xa2\x8e" // U+e88e

// Full Material Symbols PUA (ImGui 1.92 loads glyphs on demand).
inline constexpr ImWchar kMaterialSymbolsFullRange[] = {
    ICON_MIN_MS, ICON_MAX_MS,
    0,
};

// Fluent editor chrome codepoints (EditorIcons.h) — must stay on SegoeIcons.
// Material Symbols shares this PUA; exclude so Fluent wins for these.
inline constexpr ImWchar kFluentEditorIconExcludeFromMs[] = {
    0xE721, 0xE721, // Search
    0xE72C, 0xE72E, // Refresh … Lock
    0xE785, 0xE785, // Unlock
    0xE7B3, 0xE7B3, // RedEye
    0xED1A, 0xED1A, // Hide
    0,
};
