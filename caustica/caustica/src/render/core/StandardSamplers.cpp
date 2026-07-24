#include <render/core/StandardSamplers.h>

namespace caustica::render
{

StandardSamplers::StandardSamplers(caustica::rhi::Device* device)
    : m_device(device)
{
    auto samplerDesc = caustica::rhi::SamplerDesc()
        .setAllFilters(false)
        .setAllAddressModes(caustica::rhi::SamplerAddressMode::Clamp);
    m_pointClampSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(true);
    m_linearClampSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap);
    m_linearWrapSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setMaxAnisotropy(16);
    m_anisotropicWrapSampler = m_device->createSampler(samplerDesc);
}

} // namespace caustica::render
