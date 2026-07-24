#pragma once

#include <rhi/rhi.h>
#include <memory>

namespace caustica
{
    class ShaderFactory;
}

class GenerateMipsPass
{
public:
    GenerateMipsPass(
        caustica::rhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        caustica::rhi::ITexture* sourceEnvironmentMap,
        caustica::rhi::ITexture* destinationTexture);
    ~GenerateMipsPass();
    void process(caustica::rhi::ICommandList* commandList);

private:
    caustica::rhi::ComputePipelineHandle n_pipeline;
    caustica::rhi::BindingSetHandle m_bindingSet;
    caustica::rhi::TextureHandle m_sourceTexture;
    caustica::rhi::TextureHandle m_destinationTexture;
    caustica::rhi::SamplerHandle m_linearSampler;
};
