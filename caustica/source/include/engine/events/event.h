#pragma once

#include <functional>
#include <string>

namespace caustica
{

// Base event class. All events inherit from this.
class Event
{
public:
    virtual ~Event() = default;

    bool handled() const          { return m_Handled; }
    void setHandled(bool h = true) { m_Handled = h; }

    virtual const char* name() const { return "Event"; }
    virtual std::string toString() const { return name(); }

protected:
    bool m_Handled = false;
};

// Event dispatcher — invokes the appropriate handler for a given event type.
// Usage:
//   EventDispatcher dispatcher(e);
//   dispatcher.dispatch<KeyEvent>([](KeyEvent& e) { ... });
class EventDispatcher
{
public:
    explicit EventDispatcher(Event& event) : m_Event(event) {}

    template <typename T, typename F>
    bool dispatch(const F& func)
    {
        if (m_Event.handled())
            return false;

        T* derived = dynamic_cast<T*>(&m_Event);
        if (derived)
        {
            m_Event.setHandled(true);
            func(*derived);
            return true;
        }
        return false;
    }

private:
    Event& m_Event;
};

// Macro to define event type helpers
#define DECLARE_EVENT_TYPE(className, eventName)                    \
    static const char* staticType() { return eventName; }           \
    const char* name() const override { return eventName; }         \
    const char* getEventType() const { return eventName; }

} // namespace caustica
