#include <render/RTXDI/GeneratePdfMipsPass.h>
#include <assets/loader/ShaderFactory.h>
#include <rhi/utils.h>

#include <math/math.h>
#include <core/log.h>

using namespace caustica::math;

#include <shaders/render/RTXDI/ShaderParameters.h>

GenerateMipsPass::GenerateMipsPass(
    nvrhi::IDevice* device, 
    std::shared_ptr<caustica::ShaderFactory> shaderFactory,
    nvrhi::ITexture* sourceEnvironmentMap,
    nvrhi::ITexture* destinationTexture)
    : m_sourceTexture(sourceEnvironmentMap)
    , m_destinationTexture(destinationTexture)
{
    caustica::debug("Initializing GenerateMipsPass...");

    const auto& destinationDesc = m_destinationTexture->getDesc();

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setBorderColor(nvrhi::Color(0.f));
    samplerDesc.setAllFilters(true);
    samplerDesc.setMipFilter(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_linearSampler = device->createSampler(samplerDesc);

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(PreprocessEnvironmentMapConstants)),
        nvrhi::BindingSetItem::Sampler(0, m_linearSampler)
    };

    if (sourceEnvironmentMap) 
    {
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, sourceEnvironmentMap));
    };

    for (uint32_t mipLevel = 0; mipLevel < destinationDesc.mipLevels; mipLevel++)
    {
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
            mipLevel, 
            m_destinationTexture,
            nvrhi::Format::UNKNOWN, 
            nvrhi::TextureSubresourceSet(mipLevel, 1, 0, 1)));
    }

    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Compute, 0,
        bindingSetDesc, bindingLayout, m_bindingSet);

    std::vector<caustica::ShaderMacro> macros = { { "INPUT_ENVIRONMENT_MAP", sourceEnvironmentMap ? "1" : "0" } };

    nvrhi::ShaderHandle shader = shaderFactory->CreateShader("caustica/shaders/render/RTXDI/PreprocessEnvironmentMap.hlsl", "main", &macros, nvrhi::ShaderType::Compute);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { bindingLayout };
    pipelineDesc.CS = shader;
    n_pipeline = device->createComputePipeline(pipelineDesc);
}

GenerateMipsPass::~GenerateMipsPass(){}

void GenerateMipsPass::Process(nvrhi::ICommandList* commandList)
{
    commandList->beginMarker("GenerateMips");
    
    const auto& destDesc = m_destinationTexture->getDesc();

    constexpr uint32_t mipLevelsPerPass = 5;
    uint32_t width = destDesc.width;
    uint32_t height = destDesc.height;

    for (uint32_t sourceMipLevel = 0; sourceMipLevel < destDesc.mipLevels; sourceMipLevel += mipLevelsPerPass)
    {
        nvrhi::ComputeState state;
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
        
        commandList->clearState(); // make sure nvrhi inserts a barrier
    }

    commandList->endMarker();
}
