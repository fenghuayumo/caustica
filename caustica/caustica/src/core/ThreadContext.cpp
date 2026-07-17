#include <core/ThreadContext.h>

namespace caustica
{

namespace
{
thread_local ThreadDomain g_threadDomain = ThreadDomain::Logic;
}

ThreadDomain currentThreadDomain()
{
    return g_threadDomain;
}

void setCurrentThreadDomain(ThreadDomain domain)
{
    g_threadDomain = domain;
}

} // namespace caustica
