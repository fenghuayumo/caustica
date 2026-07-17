#pragma once

#include <cassert>

namespace caustica
{

enum class ThreadDomain
{
    Logic,
    Render,
};

[[nodiscard]] ThreadDomain currentThreadDomain();
void setCurrentThreadDomain(ThreadDomain domain);

class ThreadDomainScope
{
public:
    explicit ThreadDomainScope(ThreadDomain domain)
        : m_previous(currentThreadDomain())
    {
        setCurrentThreadDomain(domain);
    }

    ~ThreadDomainScope()
    {
        setCurrentThreadDomain(m_previous);
    }

    ThreadDomainScope(const ThreadDomainScope&) = delete;
    ThreadDomainScope& operator=(const ThreadDomainScope&) = delete;

private:
    ThreadDomain m_previous;
};

inline void assertLogicThread()
{
    assert(currentThreadDomain() != ThreadDomain::Render
        && "Live ECS access is forbidden from the render thread/domain");
}

inline void assertRenderThread()
{
    assert(currentThreadDomain() == ThreadDomain::Render
        && "GPU render work must run in the render thread/domain");
}

} // namespace caustica
