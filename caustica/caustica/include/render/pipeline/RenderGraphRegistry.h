#pragma once

#include <render/features/RenderFeatureContext.h>

#include <functional>
#include <vector>

namespace caustica::render
{

class RenderGraphRegistry
{
public:
    using RegistrationFn = std::function<void(RenderFeatureContext&)>;

    void add(RegistrationFn registration);
    void build(RenderFeatureContext& ctx) const;

private:
    std::vector<RegistrationFn> m_registrations;
};

} // namespace caustica::render
