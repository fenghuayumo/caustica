#pragma once

#include <cstdint>

// =============================================================================
// key_codes.h — Platform-independent key/mouse/modifier codes.
//
// Values mirror GLFW for zero-cost migration; the events layer does NOT depend
// on GLFW. Conversion from GLFW native codes happens in the window layer.
// =============================================================================

namespace caustica
{

// --- Key codes (matching GLFW_KEY_* values) ---
using KeyCode = uint16_t;

namespace Key
{
inline constexpr KeyCode Unknown      = 0;   // GLFW_KEY_UNKNOWN
inline constexpr KeyCode Space        = 32;
inline constexpr KeyCode Apostrophe   = 39;
inline constexpr KeyCode Comma        = 44;
inline constexpr KeyCode Minus        = 45;
inline constexpr KeyCode Period       = 46;
inline constexpr KeyCode Slash        = 47;
inline constexpr KeyCode Num0         = 48;
inline constexpr KeyCode Num1         = 49;
inline constexpr KeyCode Num2         = 50;
inline constexpr KeyCode Num3         = 51;
inline constexpr KeyCode Num4         = 52;
inline constexpr KeyCode Num5         = 53;
inline constexpr KeyCode Num6         = 54;
inline constexpr KeyCode Num7         = 55;
inline constexpr KeyCode Num8         = 56;
inline constexpr KeyCode Num9         = 57;
inline constexpr KeyCode Semicolon    = 59;
inline constexpr KeyCode Equal        = 61;
inline constexpr KeyCode A            = 65;
inline constexpr KeyCode B            = 66;
inline constexpr KeyCode C            = 67;
inline constexpr KeyCode D            = 68;
inline constexpr KeyCode E            = 69;
inline constexpr KeyCode F            = 70;
inline constexpr KeyCode G            = 71;
inline constexpr KeyCode H            = 72;
inline constexpr KeyCode I            = 73;
inline constexpr KeyCode J            = 74;
inline constexpr KeyCode K            = 75;
inline constexpr KeyCode L            = 76;
inline constexpr KeyCode M            = 77;
inline constexpr KeyCode N            = 78;
inline constexpr KeyCode O            = 79;
inline constexpr KeyCode P            = 80;
inline constexpr KeyCode Q            = 81;
inline constexpr KeyCode R            = 82;
inline constexpr KeyCode S            = 83;
inline constexpr KeyCode T            = 84;
inline constexpr KeyCode U            = 85;
inline constexpr KeyCode V            = 86;
inline constexpr KeyCode W            = 87;
inline constexpr KeyCode X            = 88;
inline constexpr KeyCode Y            = 89;
inline constexpr KeyCode Z            = 90;
inline constexpr KeyCode LeftBracket  = 91;
inline constexpr KeyCode Backslash    = 92;
inline constexpr KeyCode RightBracket = 93;
inline constexpr KeyCode GraveAccent  = 96;
inline constexpr KeyCode World1       = 161;
inline constexpr KeyCode World2       = 162;
inline constexpr KeyCode Escape       = 256;
inline constexpr KeyCode Enter        = 257;
inline constexpr KeyCode Tab          = 258;
inline constexpr KeyCode Backspace    = 259;
inline constexpr KeyCode Insert       = 260;
inline constexpr KeyCode Delete       = 261;
inline constexpr KeyCode Right        = 262;
inline constexpr KeyCode Left         = 263;
inline constexpr KeyCode Down         = 264;
inline constexpr KeyCode Up           = 265;
inline constexpr KeyCode PageUp       = 266;
inline constexpr KeyCode PageDown     = 267;
inline constexpr KeyCode Home         = 268;
inline constexpr KeyCode End          = 269;
inline constexpr KeyCode CapsLock     = 280;
inline constexpr KeyCode ScrollLock   = 281;
inline constexpr KeyCode NumLock      = 282;
inline constexpr KeyCode PrintScreen  = 283;
inline constexpr KeyCode Pause        = 284;
inline constexpr KeyCode F1           = 290;
inline constexpr KeyCode F2           = 291;
inline constexpr KeyCode F3           = 292;
inline constexpr KeyCode F4           = 293;
inline constexpr KeyCode F5           = 294;
inline constexpr KeyCode F6           = 295;
inline constexpr KeyCode F7           = 296;
inline constexpr KeyCode F8           = 297;
inline constexpr KeyCode F9           = 298;
inline constexpr KeyCode F10          = 299;
inline constexpr KeyCode F11          = 300;
inline constexpr KeyCode F12          = 301;
inline constexpr KeyCode F13          = 302;
inline constexpr KeyCode F14          = 303;
inline constexpr KeyCode F15          = 304;
inline constexpr KeyCode F16          = 305;
inline constexpr KeyCode F17          = 306;
inline constexpr KeyCode F18          = 307;
inline constexpr KeyCode F19          = 308;
inline constexpr KeyCode F20          = 309;
inline constexpr KeyCode F21          = 310;
inline constexpr KeyCode F22          = 311;
inline constexpr KeyCode F23          = 312;
inline constexpr KeyCode F24          = 313;
inline constexpr KeyCode F25          = 314;
inline constexpr KeyCode KP0          = 320;
inline constexpr KeyCode KP1          = 321;
inline constexpr KeyCode KP2          = 322;
inline constexpr KeyCode KP3          = 323;
inline constexpr KeyCode KP4          = 324;
inline constexpr KeyCode KP5          = 325;
inline constexpr KeyCode KP6          = 326;
inline constexpr KeyCode KP7          = 327;
inline constexpr KeyCode KP8          = 328;
inline constexpr KeyCode KP9          = 329;
inline constexpr KeyCode KPDecimal    = 330;
inline constexpr KeyCode KPDivide     = 331;
inline constexpr KeyCode KPMultiply   = 332;
inline constexpr KeyCode KPSubtract   = 333;
inline constexpr KeyCode KPAdd        = 334;
inline constexpr KeyCode KPEnter      = 335;
inline constexpr KeyCode KPEqual      = 336;
inline constexpr KeyCode LeftShift    = 340;
inline constexpr KeyCode LeftControl  = 341;
inline constexpr KeyCode LeftAlt      = 342;
inline constexpr KeyCode LeftSuper    = 343;
inline constexpr KeyCode RightShift   = 344;
inline constexpr KeyCode RightControl = 345;
inline constexpr KeyCode RightAlt     = 346;
inline constexpr KeyCode RightSuper   = 347;
inline constexpr KeyCode Menu         = 348;
} // namespace Key

// --- Mouse button codes (matching GLFW_MOUSE_BUTTON_* values) ---
using MouseCode = uint16_t;

namespace Mouse
{
inline constexpr MouseCode Button1    = 0;  // GLFW_MOUSE_BUTTON_1 (usually left)
inline constexpr MouseCode Button2    = 1;  // GLFW_MOUSE_BUTTON_2 (usually right)
inline constexpr MouseCode Button3    = 2;  // GLFW_MOUSE_BUTTON_3 (usually middle)
inline constexpr MouseCode Button4    = 3;
inline constexpr MouseCode Button5    = 4;
inline constexpr MouseCode Button6    = 5;
inline constexpr MouseCode Button7    = 6;
inline constexpr MouseCode Button8    = 7;

// Semantic aliases
inline constexpr MouseCode Left       = Button1;
inline constexpr MouseCode Right      = Button2;
inline constexpr MouseCode Middle     = Button3;
} // namespace Mouse

// --- Key action (matching GLFW_PRESS / GLFW_RELEASE / GLFW_REPEAT) ---
enum class KeyAction : uint8_t
{
    Release = 0,  // GLFW_RELEASE
    Press   = 1,  // GLFW_PRESS
    Repeat  = 2,  // GLFW_REPEAT
};

// --- Modifier key flags (matching GLFW_MOD_* values, combinable via bitwise OR) ---
enum class ModifierKey : int
{
    None    = 0,
    Shift   = 0x0001,  // GLFW_MOD_SHIFT
    Control = 0x0002,  // GLFW_MOD_CONTROL
    Alt     = 0x0004,  // GLFW_MOD_ALT
    Super   = 0x0008,  // GLFW_MOD_SUPER
    CapsLock  = 0x0010,
    NumLock   = 0x0020,
};

inline constexpr ModifierKey operator|(ModifierKey a, ModifierKey b)
{
    return static_cast<ModifierKey>(static_cast<int>(a) | static_cast<int>(b));
}
inline constexpr ModifierKey operator&(ModifierKey a, ModifierKey b)
{
    return static_cast<ModifierKey>(static_cast<int>(a) & static_cast<int>(b));
}
inline constexpr bool operator!(ModifierKey m) { return static_cast<int>(m) == 0; }
inline ModifierKey& operator|=(ModifierKey& a, ModifierKey b) { a = a | b; return a; }

} // namespace caustica
