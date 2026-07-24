#include <render/passes/postProcess/AccumulationPass.h>

#include <assets/loader/ShaderFactory.h>
#include <scene/View.h>
#include <core/log.h>

using namespace caustica::math;

#include <shaders/render/processingPasses/AccumulationPass.hlsl>

using namespace caustica;


AccumulationPass::AccumulationPass(caustica::rhi::IDevice* device, std::shared_ptr<ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(shaderFactory)
{
    caustica::rhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    bindingLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::Texture_SRV(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(1),
        caustica::rhi::BindingLayoutItem::Sampler(0),
        caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(AccumulationConstants))
    };

    m_bindingLayout = m_device->createBindingLayout(bindingLayoutDesc);

    auto samplerDesc = caustica::rhi::SamplerDesc().setAllFilters(true);

    m_sampler = m_device->createSampler(samplerDesc);
}

void AccumulationPass::createPipeline()
{
    m_computeShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/AccumulationPass.hlsl", "main", nullptr, caustica::rhi::ShaderType::Compute);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout };
    pipelineDesc.CS = m_computeShader;
    m_computePipeline = m_device->createComputePipeline(pipelineDesc);
}

void AccumulationPass::createBindingSet(caustica::rhi::ITexture* inputTexture, caustica::rhi::ITexture* outputTexture, caustica::rhi::ITexture* renderOutputTexture)
{
    caustica::rhi::BindingSetDesc bindingSetDesc;

    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::Texture_SRV(0, inputTexture),
        caustica::rhi::BindingSetItem::Texture_UAV(0, outputTexture),
        caustica::rhi::BindingSetItem::Texture_UAV(1, renderOutputTexture),
        caustica::rhi::BindingSetItem::Sampler(0, m_sampler),
        caustica::rhi::BindingSetItem::PushConstants(0, sizeof(AccumulationConstants))
    };

    m_bindingSet = m_device->createBindingSet(bindingSetDesc, m_bindingLayout);

    m_compositedColor = outputTexture;
}

void AccumulationPass::render(
    caustica::rhi::ICommandList* commandList,
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

    caustica::rhi::ComputeState state;
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
