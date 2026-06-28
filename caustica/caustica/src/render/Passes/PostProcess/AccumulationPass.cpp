#include <render/Passes/PostProcess/AccumulationPass.h>

#include <assets/loader/ShaderFactory.h>
#include <scene/View.h>
#include <core/log.h>

using namespace caustica::math;

#include <shaders/render/ProcessingPasses/AccumulationPass.hlsl>

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

void AccumulationPass::CreatePipeline()
{
    m_computeShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/AccumulationPass.hlsl", "main", nullptr, nvrhi::ShaderType::Compute);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout };
    pipelineDesc.CS = m_computeShader;
    m_computePipeline = m_device->createComputePipeline(pipelineDesc);
}

void AccumulationPass::CreateBindingSet(nvrhi::ITexture* inputTexture, nvrhi::ITexture* outputTexture, nvrhi::ITexture* renderOutputTexture)
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

void AccumulationPass::Render(
    nvrhi::ICommandList* commandList,
    const caustica::IView& sourceView,
    const caustica::IView& upscaledView,
    float accumulationWeight)
{
    commandList->beginMarker("Accumulation");

    const auto sourceViewport = sourceView.GetViewportState().viewports[0];
    const auto upscaledViewport = upscaledView.GetViewportState().viewports[0];

    const auto& inputDesc = m_compositedColor->getDesc();

    AccumulationConstants constants = {};
    constants.inputSize = float2(sourceViewport.width(), sourceViewport.height());
    constants.inputTextureSizeInv = float2(1.f / float(inputDesc.width), 1.f / float(inputDesc.height));
    constants.outputSize = float2(upscaledViewport.width(), upscaledViewport.height());
    constants.pixelOffset = sourceView.GetPixelOffset();
    constants.blendFactor = accumulationWeight;

    nvrhi::ComputeState state;
    state.bindings = { m_bindingSet };
    state.pipeline = m_computePipeline;
    commandList->setComputeState(state);

    commandList->setPushConstants(&constants, sizeof(constants));
    
    commandList->dispatch(
        dm::div_ceil(upscaledView.GetViewExtent().width(), 8), 
        dm::div_ceil(upscaledView.GetViewExtent().height(), 8), 
        1);

    commandList->endMarker();
}
