#pragma once

#include <render/FrameGraphContext.h>

#include <functional>
#include <vector>

namespace caustica::render
{

class RenderGraphRegistry
{
public:
    using RegistrationFn = std::function<void(FrameGraphContext&)>;

    void add(RegistrationFn registration);
    void build(FrameGraphContext& ctx) const;

private:
    std::vector<RegistrationFn> m_registrations;
};

} // namespace caustica::render
