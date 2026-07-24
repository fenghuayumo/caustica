#include <render/passes/rtxdi/GeneratePdfMipsPass.h>
#include <assets/loader/ShaderFactory.h>
#include <rhi/utils.h>

#include <math/math.h>
#include <core/log.h>

using namespace caustica::math;

#include <shaders/render/rtxdi/ShaderParameters.h>

GenerateMipsPass::GenerateMipsPass(
    caustica::rhi::IDevice* device, 
    std::shared_ptr<caustica::ShaderFactory> shaderFactory,
    caustica::rhi::ITexture* sourceEnvironmentMap,
    caustica::rhi::ITexture* destinationTexture)
    : m_sourceTexture(sourceEnvironmentMap)
    , m_destinationTexture(destinationTexture)
{
    caustica::debug("Initializing GenerateMipsPass...");

    const auto& destinationDesc = m_destinationTexture->getDesc();

    caustica::rhi::SamplerDesc samplerDesc;
    samplerDesc.setBorderColor(caustica::rhi::Color(0.f));
    samplerDesc.setAllFilters(true);
    samplerDesc.setMipFilter(true);
    samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap);
    m_linearSampler = device->createSampler(samplerDesc);

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::PushConstants(0, sizeof(PreprocessEnvironmentMapConstants)),
        caustica::rhi::BindingSetItem::Sampler(0, m_linearSampler)
    };

    if (sourceEnvironmentMap) 
    {
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(0, sourceEnvironmentMap));
    };

    for (uint32_t mipLevel = 0; mipLevel < destinationDesc.mipLevels; mipLevel++)
    {
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(
            mipLevel, 
            m_destinationTexture,
            caustica::rhi::Format::UNKNOWN, 
            caustica::rhi::TextureSubresourceSet(mipLevel, 1, 0, 1)));
    }

    caustica::rhi::BindingLayoutHandle bindingLayout;
    caustica::rhi::utils::CreateBindingSetAndLayout(device, caustica::rhi::ShaderType::Compute, 0,
        bindingSetDesc, bindingLayout, m_bindingSet);

    std::vector<caustica::ShaderMacro> macros = { { "INPUT_ENVIRONMENT_MAP", sourceEnvironmentMap ? "1" : "0" } };

    caustica::rhi::ShaderHandle shader = shaderFactory->createShader("caustica/shaders/render/rtxdi/PreprocessEnvironmentMap.hlsl", "main", &macros, caustica::rhi::ShaderType::Compute);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { bindingLayout };
    pipelineDesc.CS = shader;
    n_pipeline = device->createComputePipeline(pipelineDesc);
}

GenerateMipsPass::~GenerateMipsPass(){}

void GenerateMipsPass::process(caustica::rhi::ICommandList* commandList)
{
    commandList->beginMarker("generateMips");
    
    const auto& destDesc = m_destinationTexture->getDesc();

    constexpr uint32_t mipLevelsPerPass = 5;
    uint32_t width = destDesc.width;
    uint32_t height = destDesc.height;

    for (uint32_t sourceMipLevel = 0; sourceMipLevel < destDesc.mipLevels; sourceMipLevel += mipLevelsPerPass)
    {
        caustica::rhi::ComputeState state;
        state.pipeline = n_pipeline;
        state.bindings = { m_bindingSet };
        commandList->setComputeState(state);

        PreprocessEnvironmentMapConstants constants{};
        constants.sourceSize = { destDesc.width, destDesc.height };
        constants.numDestMipLevels = destDesc.mipLevels;
        constants.sourceMipLevel = sourceMipLevel;
        commandList->setPushConstants(&constants, sizeof(constants));

        commandList->dispatch(div_ceil(width, 32), div_ceil(height, 32), 1);

        width = std::max(1u, width >> mipLevelsPerPass);
        height = std::max(1u, height >> mipLevelsPerPass);
        
        commandList->clearState(); // make sure RHI inserts a barrier
    }

    commandList->endMarker();
}
