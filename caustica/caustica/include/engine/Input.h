#pragma once

#include <events/key_codes.h>

#include <unordered_set>
#include <vector>

namespace caustica
{

template<typename T>
struct ButtonInput
{
    [[nodiscard]] bool pressed(T button) const { return m_pressed.contains(button); }
    [[nodiscard]] bool justPressed(T button) const { return m_justPressed.contains(button); }
    [[nodiscard]] bool justReleased(T button) const { return m_justReleased.contains(button); }

    void press(T button)
    {
        if (m_pressed.insert(button).second)
            m_justPressed.insert(button);
        m_justReleased.erase(button);
    }

    void release(T button)
    {
        if (m_pressed.erase(button) > 0)
            m_justReleased.insert(button);
        m_justPressed.erase(button);
    }

    void clearTransitions()
    {
        m_justPressed.clear();
        m_justReleased.clear();
    }

    template<typename Fn>
    void forEachJustPressed(Fn&& fn) const
    {
        for (T button : m_justPressed)
            fn(button);
    }

    template<typename Fn>
    void forEachJustReleased(Fn&& fn) const
    {
        for (T button : m_justReleased)
            fn(button);
    }

private:
    std::unordered_set<T> m_pressed;
    std::unordered_set<T> m_justPressed;
    std::unordered_set<T> m_justReleased;
};

struct Cursor
{
    double x = 0.0;
    double y = 0.0;
    double deltaX = 0.0;
    double deltaY = 0.0;
};

// Per-frame window input, filled from engine events in First.
struct InputState
{
    ButtonInput<KeyCode> keys;
    ButtonInput<MouseCode> mouse;
    Cursor cursor;
    ModifierKey modifiers = ModifierKey::None;
    double scrollX = 0.0;
    double scrollY = 0.0;
    std::vector<unsigned int> text;

    void beginFrame()
    {
        keys.clearTransitions();
        mouse.clearTransitions();
        cursor.deltaX = 0.0;
        cursor.deltaY = 0.0;
        scrollX = 0.0;
        scrollY = 0.0;
        text.clear();
    }

    [[nodiscard]] bool keyDown(KeyCode key) const { return keys.pressed(key); }
    [[nodiscard]] bool shift() const
    {
        return (static_cast<int>(modifiers) & static_cast<int>(ModifierKey::Shift)) != 0
            || keys.pressed(Key::LeftShift) || keys.pressed(Key::RightShift);
    }
    [[nodiscard]] bool control() const
    {
        return (static_cast<int>(modifiers) & static_cast<int>(ModifierKey::Control)) != 0
            || keys.pressed(Key::LeftControl) || keys.pressed(Key::RightControl);
    }
    [[nodiscard]] bool alt() const
    {
        return (static_cast<int>(modifiers) & static_cast<int>(ModifierKey::Alt)) != 0
            || keys.pressed(Key::LeftAlt) || keys.pressed(Key::RightAlt);
    }
};

// Default fly/look mapping. Editor tightens this (RMB-fly, Alt+LMB look).
struct CameraInputConfig
{
    bool flyRequiresRightMouse = false;
    bool lookWithAltLeft = false;
    bool lookWithRightMouse = true;
    bool panWithMiddleMouse = true;
};

// Per-frame gate. Editor / game camera clear flags when they own the view.
struct CameraInputGate
{
    bool enabled = true;
    bool fly = true;
    bool look = true;
    bool pan = true;
    bool scroll = true;
    bool keyboard = true;
};

class App;
class Event;

void applyEventToInput(InputState& input, const Event& event);
void applyCameraWindowInput(App& app);

} // namespace caustica
