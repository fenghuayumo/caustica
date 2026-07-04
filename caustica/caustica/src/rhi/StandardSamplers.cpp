#include <rhi/StandardSamplers.h>

namespace caustica::rhi
{

StandardSamplers::StandardSamplers(nvrhi::IDevice* device)
    : m_Device(device)
{
    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllFilters(false)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    m_PointClampSampler = m_Device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(true);
    m_LinearClampSampler = m_Device->createSampler(samplerDesc);

    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_LinearWrapSampler = m_Device->createSampler(samplerDesc);

    samplerDesc.setMaxAnisotropy(16);
    m_AnisotropicWrapSampler = m_Device->createSampler(samplerDesc);
}

} // namespace caustica::rhi
