#include <render/passes/postProcess/AccumulationPass.h>

#include <assets/loader/ShaderFactory.h>
#include <scene/View.h>
#include <core/log.h>

using namespace caustica::math;

#include <shaders/render/processingPasses/AccumulationPass.hlsl>

using namespace caustica;


AccumulationPass::AccumulationPass(nvrhi::IDevice* device, std::shared_ptr<ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(shaderFactory)
{
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    bindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_UAV(0),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AccumulationConstants))
    };

    m_bindingLayout = m_device->createBindingLayout(bindingLayoutDesc);

    auto samplerDesc = nvrhi::SamplerDesc().setAllFilters(true);

    m_sampler = m_device->createSampler(samplerDesc);
}

void AccumulationPass::createPipeline()
{
    m_computeShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/AccumulationPass.hlsl", "main", nullptr, nvrhi::ShaderType::Compute);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout };
    pipelineDesc.CS = m_computeShader;
    m_computePipeline = m_device->createComputePipeline(pipelineDesc);
}

void AccumulationPass::createBindingSet(nvrhi::ITexture* inputTexture, nvrhi::ITexture* outputTexture, nvrhi::ITexture* renderOutputTexture)
{
    nvrhi::BindingSetDesc bindingSetDesc;

    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(0, inputTexture),
        nvrhi::BindingSetItem::Texture_UAV(0, outputTexture),
        nvrhi::BindingSetItem::Texture_UAV(1, renderOutputTexture),
        nvrhi::BindingSetItem::Sampler(0, m_sampler),
        nvrhi::BindingSetItem::PushConstants(0, sizeof(AccumulationConstants))
    };

    m_bindingSet = m_device->createBindingSet(bindingSetDesc, m_bindingLayout);

    m_compositedColor = outputTexture;
}

void AccumulationPass::render(
    nvrhi::ICommandList* commandList,
    const caustica::IView& sourceView,
    const caustica::IView& upscaledView,
    float accumulationWeight)
{
    commandList->beginMarker("Accumulation");

    const auto sourceViewport = sourceView.getViewportState().viewports[0];
    const auto upscaledViewport = upscaledView.getViewportState().viewports[0];

    const auto& inputDesc = m_compositedColor->getDesc();

    AccumulationConstants constants = {};
    constants.inputSize = float2(sourceViewport.width(), sourceViewport.height());
    constants.inputTextureSizeInv = float2(1.f / float(inputDesc.width), 1.f / float(inputDesc.height));
    constants.outputSize = float2(upscaledViewport.width(), upscaledViewport.height());
    constants.pixelOffset = sourceView.getPixelOffset();
    constants.blendFactor = accumulationWeight;

    nvrhi::ComputeState state;
    state.bindings = { m_bindingSet };
    state.pipeline = m_computePipeline;
    commandList->setComputeState(state);

    commandList->setPushConstants(&constants, sizeof(constants));
    
    commandList->dispatch(
        dm::div_ceil(upscaledView.getViewExtent().width(), 8), 
        dm::div_ceil(upscaledView.getViewExtent().height(), 8), 
        1);

    commandList->endMarker();
}
