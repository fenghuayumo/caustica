#pragma once

#include <rhi/rhi.h>
#include <memory>

namespace caustica
{
    class ShaderFactory;
    class IView;
}

class AccumulationPass
{
public:
    AccumulationPass(caustica::rhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);

    void createPipeline();
    void createBindingSet(caustica::rhi::ITexture* inputTexture, caustica::rhi::ITexture* outputTexture, caustica::rhi::ITexture* renderOutputTexture);
    void render(caustica::rhi::ICommandList* commandList, const caustica::IView& sourceView, const caustica::IView& upscaledView, float accumulationWeight);

private:
    caustica::rhi::DeviceHandle m_device;
    caustica::rhi::ShaderHandle m_computeShader;
    caustica::rhi::ComputePipelineHandle m_computePipeline;
    caustica::rhi::BindingLayoutHandle m_bindingLayout;
    caustica::rhi::BindingSetHandle m_bindingSet;
    caustica::rhi::SamplerHandle m_sampler;
    caustica::rhi::TextureHandle m_compositedColor;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
};
