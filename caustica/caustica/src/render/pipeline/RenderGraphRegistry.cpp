#include <render/pipeline/RenderGraphRegistry.h>

namespace caustica::render
{

void RenderGraphRegistry::add(RegistrationFn registration)
{
    if (registration)
        m_registrations.push_back(std::move(registration));
}

void RenderGraphRegistry::build(FrameGraphContext& ctx) const
{
    for (const RegistrationFn& registration : m_registrations)
        registration(ctx);
}

} // namespace caustica::render
