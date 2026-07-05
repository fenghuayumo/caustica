#pragma once

#include <rhi/nvrhi.h>

namespace caustica::render
{

class StandardSamplers
{
public:
    explicit StandardSamplers(nvrhi::IDevice* device);

    nvrhi::SamplerHandle pointClamp() const { return m_PointClampSampler; }
    nvrhi::SamplerHandle linearClamp() const { return m_LinearClampSampler; }
    nvrhi::SamplerHandle linearWrap() const { return m_LinearWrapSampler; }
    nvrhi::SamplerHandle anisotropicWrap() const { return m_AnisotropicWrapSampler; }

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::SamplerHandle m_PointClampSampler;
    nvrhi::SamplerHandle m_LinearClampSampler;
    nvrhi::SamplerHandle m_LinearWrapSampler;
    nvrhi::SamplerHandle m_AnisotropicWrapSampler;
};

} // namespace caustica::render
