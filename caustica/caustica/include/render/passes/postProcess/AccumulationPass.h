#pragma once

#include <rhi/nvrhi.h>
#include <memory>

namespace caustica
{
    class ShaderFactory;
    class IView;
}

class AccumulationPass
{
public:
    AccumulationPass(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);

    void CreatePipeline();
    void CreateBindingSet(nvrhi::ITexture* inputTexture, nvrhi::ITexture* outputTexture, nvrhi::ITexture* renderOutputTexture);
    void Render(nvrhi::ICommandList* commandList, const caustica::IView& sourceView, const caustica::IView& upscaledView, float accumulationWeight);

private:
    nvrhi::DeviceHandle m_device;
    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::TextureHandle m_compositedColor;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
};
