#pragma once

#include <rhi/nvrhi.h>

namespace caustica::render
{

class StandardSamplers
{
public:
    explicit StandardSamplers(nvrhi::IDevice* device);

    nvrhi::SamplerHandle pointClamp() const { return m_pointClampSampler; }
    nvrhi::SamplerHandle linearClamp() const { return m_linearClampSampler; }
    nvrhi::SamplerHandle linearWrap() const { return m_linearWrapSampler; }
    nvrhi::SamplerHandle anisotropicWrap() const { return m_anisotropicWrapSampler; }

private:
    nvrhi::DeviceHandle m_device;
    nvrhi::SamplerHandle m_pointClampSampler;
    nvrhi::SamplerHandle m_linearClampSampler;
    nvrhi::SamplerHandle m_linearWrapSampler;
    nvrhi::SamplerHandle m_anisotropicWrapSampler;
};

} // namespace caustica::render
