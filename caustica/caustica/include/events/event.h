#pragma once

#include "events/key_codes.h"

#include <functional>
#include <string>

// =============================================================================
// event.h — Event base class, type/category enums, EventDispatcher template.
//
// Pattern: DIVSHOT's events layer. Each concrete event implements
// GetEventType() + GetCategoryFlags() and uses the helper macros below.
// The EventDispatcher provides type-safe, category-filtered dispatch.
// =============================================================================

namespace caustica
{

// ---------------------------------------------------------------------------
// EventType — one entry per concrete event class
// ---------------------------------------------------------------------------
enum class EventType : uint16_t
{
    None = 0,

    // --- Keyboard ---
    KeyPressed,
    KeyReleased,
    KeyTyped,

    // --- Mouse ---
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,

    // --- Application / Window ---
    WindowClose,
    WindowResize,
    WindowFocus,
    WindowLostFocus,
    WindowMoved,
    WindowIconify,
    AppTick,
    AppUpdate,
    AppRender,

    // --- Sentinel ---
    Count
};

// ---------------------------------------------------------------------------
// EventCategory — bitmask flags (combinable)
// ---------------------------------------------------------------------------
enum class EventCategory : uint16_t
{
    None        = 0,
    Application = 1 << 0,
    Input       = 1 << 1,
    Keyboard    = 1 << 2,
    Mouse       = 1 << 3,
    MouseButton = 1 << 4,
};

inline constexpr EventCategory operator|(EventCategory a, EventCategory b)
{
    return static_cast<EventCategory>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline constexpr EventCategory operator&(EventCategory a, EventCategory b)
{
    return static_cast<EventCategory>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
inline constexpr bool operator!(EventCategory c) { return static_cast<uint16_t>(c) == 0; }

// ---------------------------------------------------------------------------
// Event — abstract base for all dispatched events
// ---------------------------------------------------------------------------
class Event
{
public:
    virtual ~Event() = default;

    virtual EventType    GetEventType()     const = 0;
    virtual const char*  GetName()          const = 0;
    virtual int          GetCategoryFlags() const = 0;
    virtual std::string  ToString()         const { return GetName(); }

    bool Handled() const { return m_Handled; }
    void SetHandled(bool h = true) { m_Handled = h; }

protected:
    bool m_Handled = false;
};

// ---------------------------------------------------------------------------
// EventDispatcher — type-safe dynamic dispatch.
//
// Usage:
//   void onEvent(Event& e) {
//       EventDispatcher dispatcher(e);
//       dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FN(onKeyPressed));
//       dispatcher.Dispatch<WindowResizeEvent>([&](auto& ev) { ... });
//   }
// ---------------------------------------------------------------------------
template<typename T>
concept EventTypeConcept = std::derived_from<T, Event>;

class EventDispatcher
{
public:
    explicit EventDispatcher(Event& event)
        : m_Event(event)
    {}

    // Dispatch to a handler of concrete type T. Returns true if handled.
    template<EventTypeConcept T>
    bool Dispatch(std::function<bool(T&)> handler)
    {
        if (m_Event.GetEventType() != T::GetStaticType())
            return false;

        bool handled = handler(static_cast<T&>(m_Event));
        if (handled)
            m_Event.SetHandled(true);
        return handled;
    }

    // Dispatch only if the event category matches.
    template<EventTypeConcept T>
    bool DispatchIf(EventCategory category, std::function<bool(T&)> handler)
    {
        if (m_Event.GetEventType() != T::GetStaticType())
            return false;

        const int cat = static_cast<int>(category);
        if ((m_Event.GetCategoryFlags() & cat) == 0)
            return false;

        bool handled = handler(static_cast<T&>(m_Event));
        if (handled)
            m_Event.SetHandled(true);
        return handled;
    }

private:
    Event& m_Event;
};

// ---------------------------------------------------------------------------
// Convenience macros for defining concrete event types
// ---------------------------------------------------------------------------
#define EVENT_CLASS_TYPE(eventType) \
    static EventType GetStaticType() { return EventType::eventType; } \
    EventType GetEventType() const override { return GetStaticType(); }

#define EVENT_CLASS_CATEGORY(category) \
    int GetCategoryFlags() const override { return static_cast<int>(EventCategory::category); }

// For events spanning multiple categories
#define EVENT_CLASS_CATEGORY_FLAGS(...) \
    int GetCategoryFlags() const override { return static_cast<int>((__VA_ARGS__)); }

// Binds a member function as an std::function for EventDispatcher
#define BIND_EVENT_FN(fn) \
    [this](auto& e) -> bool { return this->fn(e); }

} // namespace caustica
