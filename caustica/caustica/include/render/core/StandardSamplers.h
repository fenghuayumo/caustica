#pragma once

#include <rhi/rhi.h>

namespace caustica::render
{

class StandardSamplers
{
public:
    explicit StandardSamplers(caustica::rhi::Device* device);

    caustica::rhi::SamplerHandle pointClamp() const { return m_pointClampSampler; }
    caustica::rhi::SamplerHandle linearClamp() const { return m_linearClampSampler; }
    caustica::rhi::SamplerHandle linearWrap() const { return m_linearWrapSampler; }
    caustica::rhi::SamplerHandle anisotropicWrap() const { return m_anisotropicWrapSampler; }

private:
    caustica::rhi::DeviceHandle m_device;
    caustica::rhi::SamplerHandle m_pointClampSampler;
    caustica::rhi::SamplerHandle m_linearClampSampler;
    caustica::rhi::SamplerHandle m_linearWrapSampler;
    caustica::rhi::SamplerHandle m_anisotropicWrapSampler;
};

} // namespace caustica::render
