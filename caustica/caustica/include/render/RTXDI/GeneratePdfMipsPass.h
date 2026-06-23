#pragma once

#include <rhi/nvrhi.h>
#include <memory>

namespace caustica
{
    class ShaderFactory;
}

class GenerateMipsPass
{
public:
    GenerateMipsPass(
        nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        nvrhi::ITexture* sourceEnvironmentMap,
        nvrhi::ITexture* destinationTexture);
    ~GenerateMipsPass();
    void Process(nvrhi::ICommandList* commandList);

private:
    nvrhi::ComputePipelineHandle n_pipeline;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::TextureHandle m_sourceTexture;
    nvrhi::TextureHandle m_destinationTexture;
    nvrhi::SamplerHandle m_linearSampler;
};
