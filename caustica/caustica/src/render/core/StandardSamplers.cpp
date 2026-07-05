#include <render/core/StandardSamplers.h>

namespace caustica::render
{

StandardSamplers::StandardSamplers(nvrhi::IDevice* device)
    : m_device(device)
{
    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllFilters(false)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    m_pointClampSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(true);
    m_linearClampSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_linearWrapSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setMaxAnisotropy(16);
    m_anisotropicWrapSampler = m_device->createSampler(samplerDesc);
}

} // namespace caustica::render
