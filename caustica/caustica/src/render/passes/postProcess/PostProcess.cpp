#include <render/passes/postProcess/PostProcess.h>

#include <render/core/FramebufferFactory.h>
#include <render/core/RenderDevice.h>
#include <render/passes/debug/ShaderDebug.h>

using namespace caustica::math;
using namespace caustica;

PostProcess::PostProcess( caustica::rhi::Device* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory, 
    caustica::render::RenderDevice& renderDevice, std::shared_ptr<ShaderDebug> shaderDebug
    )
    : m_device(device)
    , m_renderDevice(renderDevice)
    , m_bindingCache(device)
    , m_shaderDebug(shaderDebug)
{

    for (uint32_t i = 0; i < (uint32_t)ComputePassType::MaxCount; i++)
    {
        std::vector<caustica::ShaderMacro> shaderMacros;
        switch ((ComputePassType)i)
        {
        case(ComputePassType::StablePlanesDebugViz):
            shaderMacros.push_back(caustica::ShaderMacro({ "STABLE_PLANES_DEBUG_VIZ", "1" }));
            break;
        case(ComputePassType::RELAXDenoiserPrepareInputs):
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_PREPARE_INPUTS", "1" }));
            shaderMacros.push_back(caustica::ShaderMacro({ "USE_RELAX", "1" }));
            break;
        case(ComputePassType::REBLURDenoiserPrepareInputs):
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_PREPARE_INPUTS", "1" }));
            shaderMacros.push_back(caustica::ShaderMacro({ "USE_RELAX", "0" }));
            break;
        case(ComputePassType::RELAXDenoiserFinalMerge): 
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_FINAL_MERGE", "1" })); 
            shaderMacros.push_back(caustica::ShaderMacro({ "USE_RELAX", "1" }));
            break;
        case(ComputePassType::REBLURDenoiserFinalMerge): 
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_FINAL_MERGE", "1" }));
            shaderMacros.push_back(caustica::ShaderMacro({ "USE_RELAX", "0" }));
            break;
        case(ComputePassType::DLSSRRDenoiserPrepareInputs):
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_PREPARE_INPUTS", "1" }));
            shaderMacros.push_back(caustica::ShaderMacro({ "DENOISER_DLSS_RR", "1" }));
            break;
        case(ComputePassType::NoDenoiserFinalMerge):
            shaderMacros.push_back(caustica::ShaderMacro({ "NO_DENOISER_FINAL_MERGE", "1" }));
            break;
        case(ComputePassType::DummyPlaceholder): shaderMacros.push_back(caustica::ShaderMacro({ "DUMMY_PLACEHOLDER_EFFECT", "1" })); break;
        };
        m_computeShaders[i] = shaderFactory->createShader("caustica/shaders/render/processingPasses/PostProcess.hlsl", "main", &shaderMacros, caustica::rhi::ShaderType::Compute);
    }
    //m_MainCS = shaderFactory->createShader("caustica/shaders/render/processingPasses/PostProcess.hlsl", "main", &std::vector<caustica::ShaderMacro>(1, caustica::ShaderMacro("USE_CS", "1")), caustica::rhi::ShaderType::Compute);

    caustica::rhi::BindingLayoutDesc layoutDesc;

    layoutDesc.visibility = caustica::rhi::ShaderType::Compute | caustica::rhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::PushConstants(1, sizeof(FrameMiniConstants)),
        caustica::rhi::BindingLayoutItem::Texture_SRV(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(0),
        caustica::rhi::BindingLayoutItem::Texture_SRV(2),
        caustica::rhi::BindingLayoutItem::Texture_SRV(3),
        caustica::rhi::BindingLayoutItem::Texture_SRV(4),
        caustica::rhi::BindingLayoutItem::Texture_SRV(5),
        caustica::rhi::BindingLayoutItem::Texture_SRV(6),
        caustica::rhi::BindingLayoutItem::Texture_SRV(7),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(10),
        caustica::rhi::BindingLayoutItem::Sampler(0),
        caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        caustica::rhi::BindingLayoutItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX),
    };
    m_bindingLayoutCS = m_device->createBindingLayout(layoutDesc);

    caustica::rhi::SamplerDesc samplerDesc;
    samplerDesc.setBorderColor(caustica::rhi::Color(0.f));
    samplerDesc.setAllFilters(true);
    samplerDesc.setMipFilter(false);
    samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap);
    m_linearSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(false);
    m_pointSampler = m_device->createSampler(samplerDesc);
}

void PostProcess::apply(caustica::rhi::CommandList* commandList, ComputePassType passType, caustica::rhi::BufferHandle consts, FrameMiniConstants & miniConsts, caustica::rhi::BindingSetHandle bindingSet, caustica::rhi::BindingLayoutHandle bindingLayout, uint32_t width, uint32_t height)
{
    uint passIndex = (uint32_t)passType;

    if (m_computePSOs[passIndex] == nullptr)
    {
        caustica::rhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { bindingLayout };
        pipelineDesc.CS = m_computeShaders[passIndex];
        m_computePSOs[passIndex] = m_device->createComputePipeline(pipelineDesc);
    }

    caustica::rhi::ComputeState state;
    state.bindings = { bindingSet };
    state.pipeline = m_computePSOs[passIndex];

    commandList->setComputeState(state);

    const dm::uint  threads = NUM_COMPUTE_THREADS_PER_DIM;
    const dm::uint2 dispatchSize = dm::uint2((width + threads - 1) / threads, (height + threads - 1) / threads);
    commandList->setPushConstants(&miniConsts, sizeof(miniConsts));
    commandList->dispatch(dispatchSize.x, dispatchSize.y);
}

void PostProcess::apply( caustica::rhi::CommandList* commandList, ComputePassType passType, int pass, caustica::rhi::BufferHandle consts, FrameMiniConstants & miniConsts, caustica::rhi::Texture* workTexture, RenderTargets & renderTargets, caustica::rhi::Texture* sourceTexture)
{
    // commandList->beginMarker("PostProcessCS");

    assert((uint32_t)passType >= 0 && passType < ComputePassType::MaxCount);

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
    		caustica::rhi::BindingSetItem::ConstantBuffer(0, consts),
            caustica::rhi::BindingSetItem::PushConstants(1, sizeof(FrameMiniConstants)), 
            caustica::rhi::BindingSetItem::Texture_SRV(0, (sourceTexture!=nullptr)?(sourceTexture):(m_renderDevice.builtins().whiteTexture().Get())),
            caustica::rhi::BindingSetItem::Texture_UAV(0, workTexture),
    		//caustica::rhi::BindingSetItem::StructuredBuffer_SRV(1, renderTargets.DenoiserPixelDataBuffer),
            caustica::rhi::BindingSetItem::Texture_SRV(2, renderTargets.denoiserOutDiffRadianceHitDist[pass]),
            caustica::rhi::BindingSetItem::Texture_SRV(3, renderTargets.denoiserOutSpecRadianceHitDist[pass]),
            caustica::rhi::BindingSetItem::Texture_SRV(4, m_renderDevice.builtins().whiteTexture().Get()),
            caustica::rhi::BindingSetItem::Texture_SRV(5, (renderTargets.denoiserOutValidation!=nullptr)?(renderTargets.denoiserOutValidation):((caustica::rhi::TextureHandle)m_renderDevice.builtins().whiteTexture().Get())),
            caustica::rhi::BindingSetItem::Texture_SRV(6, renderTargets.denoiserViewspaceZ),
            caustica::rhi::BindingSetItem::Texture_SRV(7, renderTargets.denoiserDisocclusionThresholdMix),
            caustica::rhi::BindingSetItem::StructuredBuffer_SRV(10, renderTargets.stablePlanesBuffer),
            caustica::rhi::BindingSetItem::Sampler(0, (true) ? m_linearSampler : m_pointSampler),
            caustica::rhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
            caustica::rhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->getDebugVizTexture()),
    	};

    caustica::rhi::BindingSetHandle bindingSet = m_bindingCache.getOrCreateBindingSet(bindingSetDesc, m_bindingLayoutCS);

    apply(commandList, passType, consts, miniConsts, bindingSet, m_bindingLayoutCS, workTexture->getDesc().width, workTexture->getDesc().height);

    // commandList->endMarker();
}

