#pragma once

#include <vector>

namespace caustica::ecs
{

// Per-event-type queue stored as a World resource (Bevy-style Events<E>).
template<typename E>
struct Events
{
    std::vector<E> events;

    void send(const E& event) { events.push_back(event); }
    void send(E&& event) { events.push_back(std::move(event)); }

    void clear() { events.clear(); }

    [[nodiscard]] bool empty() const { return events.empty(); }

    auto begin() { return events.begin(); }
    auto end() { return events.end(); }
    auto begin() const { return events.begin(); }
    auto end() const { return events.end(); }
};

template<typename E>
class EventWriter
{
public:
    explicit EventWriter(Events<E>* events = nullptr)
        : m_events(events)
    {
    }

    void send(const E& event)
    {
        if (m_events)
            m_events->send(event);
    }

    void send(E&& event)
    {
        if (m_events)
            m_events->send(std::move(event));
    }

private:
    Events<E>* m_events = nullptr;
};

template<typename E>
class EventReader
{
public:
    explicit EventReader(const Events<E>* events = nullptr)
        : m_events(events)
    {
    }

    [[nodiscard]] const std::vector<E>& read() const
    {
        static const std::vector<E> kEmpty;
        return m_events ? m_events->events : kEmpty;
    }

    [[nodiscard]] bool empty() const { return !m_events || m_events->empty(); }

private:
    const Events<E>* m_events = nullptr;
};

} // namespace caustica::ecs
