#include <engine/SubsystemCollection.h>

#include <algorithm>

namespace caustica
{

void SubsystemCollection::add(std::unique_ptr<ISubsystem> subsystem)
{
    if (!subsystem)
        return;

    ISubsystem* raw = subsystem.get();
    m_byType[std::type_index(typeid(*raw))] = raw;
    m_subsystems.push_back(std::move(subsystem));

    std::stable_sort(m_subsystems.begin(), m_subsystems.end(),
        [](const std::unique_ptr<ISubsystem>& a, const std::unique_ptr<ISubsystem>& b) {
            return a->priority() < b->priority();
        });
}

bool SubsystemCollection::initializeAll(EngineInitContext& context)
{
    if (m_initialized)
        return true;

    context.subsystems = this;
    for (const auto& subsystem : m_subsystems)
    {
        if (subsystem)
            subsystem->initialize(context);
    }

    m_initialized = true;
    return true;
}

void SubsystemCollection::shutdownAll()
{
    if (!m_initialized)
        return;

    for (auto it = m_subsystems.rbegin(); it != m_subsystems.rend(); ++it)
    {
        if (*it)
            (*it)->shutdown();
    }

    m_initialized = false;
}

void SubsystemCollection::onBackBufferResizing()
{
    for (const auto& subsystem : m_subsystems)
    {
        if (subsystem)
            subsystem->onBackBufferResizing();
    }
}

void SubsystemCollection::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    for (const auto& subsystem : m_subsystems)
    {
        if (subsystem)
            subsystem->onBackBufferResized(width, height, sampleCount);
    }
}

} // namespace caustica
