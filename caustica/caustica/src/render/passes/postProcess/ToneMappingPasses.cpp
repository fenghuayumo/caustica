#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderDevice.h>
#include <backend/ViewRhiConversion.h>
#include <scene/View.h>
#include <sstream>
#include <algorithm>
#include <assert.h>
#include <stdexcept>
#include <render/core/FramebufferFactory.h>
#include <core/log.h>

using namespace caustica::math;
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <shaders/render/toneMapper/ToneMapping_cb.h>
#include <render/passes/postProcess/ColorUtils.h>

using namespace caustica;
using namespace caustica::render;

#ifndef TONEMAPPING_AUTOEXPOSURE_CPU
#error this must be defined
#endif

ToneMappingPass::ToneMappingPass(
    caustica::rhi::Device* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory,
    caustica::render::RenderDevice& renderDevice,
    std::shared_ptr<caustica::FramebufferFactory> colorFramebufferFactory,
    const caustica::ICompositeView& compositeView,
	caustica::rhi::TextureHandle sourceTexture)
    : m_device(device)
    , m_renderDevice(renderDevice)
    , m_FramebufferFactory(colorFramebufferFactory)
{
    const IView* sampleView = compositeView.getChildView(ViewType::PLANAR, 0);
    caustica::rhi::Framebuffer* colorSampleFramebuffer = m_FramebufferFactory->getFramebuffer(*sampleView);
    {
        m_LuminanceShader = shaderFactory->createShader("caustica/shaders/render/toneMapper/luminance_ps.hlsl", "main", nullptr, caustica::rhi::ShaderType::Pixel);
        m_ToneMapShader = shaderFactory->createShader("caustica/shaders/render/toneMapper/ToneMapping.hlsl", "main_ps", nullptr, caustica::rhi::ShaderType::Pixel);
#if TONEMAPPING_AUTOEXPOSURE_CPU
        m_CaptureLuminanceShader = shaderFactory->createShader("caustica/shaders/render/toneMapper/ToneMapping.hlsl", "capture_cs", nullptr, caustica::rhi::ShaderType::Compute);
#endif
    }

    caustica::rhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(ToneMappingConstants);
    constantBufferDesc.debugName = "ToneMappingConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = 16;// params.numConstantBufferVersions;
    m_ToneMappingCB = m_device->createBuffer(constantBufferDesc);

    caustica::rhi::SamplerDesc samplerDesc;
    samplerDesc.setBorderColor(caustica::rhi::Color(0.f));
    samplerDesc.setAllFilters(true);
    samplerDesc.setMipFilter(false);
    samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap);
    m_linearSampler = m_device->createSampler(samplerDesc);

	samplerDesc.setAllFilters(false);
	m_pointSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Clamp);
    m_cameraLutSampler = m_device->createSampler(samplerDesc);
    // The 3D LUT slot is part of the shader's fixed binding layout. Keep it
    // valid while LUTs are disabled or a 1D LUT is active; the shader only
    // samples this fallback when cameraLutIs3D is set.
    m_cameraLut3DTexture = m_renderDevice.builtins().blackTexture3D();


    m_PerView.resize(compositeView.getNumChildViews(ViewType::PLANAR));
    {
        for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);
            caustica::rhi::Framebuffer* sampleFrameBuffer = m_FramebufferFactory->getFramebuffer(*view);
            PerViewData& perViewData = m_PerView[viewIndex];

            ScissorDesc viewExtent = view->getViewExtent();
            uint32_t viewportWidth = viewExtent.maxX - viewExtent.minX;
            uint32_t viewportHeight = viewExtent.maxY - viewExtent.minY;

            caustica::rhi::Format format = sourceTexture->getDesc().format == caustica::rhi::Format::RGBA32_FLOAT ? 
                caustica::rhi::Format::R32_FLOAT : caustica::rhi::Format::R16_FLOAT;

            caustica::rhi::TextureDesc luminanceTextureDesc;
            luminanceTextureDesc.format = format;
            luminanceTextureDesc.width = 1U << (int)log2(viewportWidth); //Lower to nearest power of 2
            luminanceTextureDesc.height = 1U << (int)log2(viewportHeight);
            uint32_t dims = viewportWidth | viewportHeight;
            luminanceTextureDesc.mipLevels = (uint32_t)(log2(dims) + 1); //Calculate the number of mip levels required
            luminanceTextureDesc.isRenderTarget = true;
            luminanceTextureDesc.isUAV = true;
            luminanceTextureDesc.debugName = "Luminance Texture";
            luminanceTextureDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
            luminanceTextureDesc.keepInitialState = true;
            perViewData.luminanceTexture = m_device->createTexture(luminanceTextureDesc);
            perViewData.luminanceFrameBuffer = m_device->createFramebuffer(
                caustica::rhi::FramebufferDesc().addColorAttachment(perViewData.luminanceTexture));

#if TONEMAPPING_AUTOEXPOSURE_CPU
            // readback for luminance coming out of tonemapper so we can set exposure on the CPU side
            {
                caustica::rhi::BufferDesc bufferDesc;
                bufferDesc.byteSize = 4;
                bufferDesc.format = caustica::rhi::Format::R32_FLOAT;
                bufferDesc.canHaveUAVs = true;
                bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
                bufferDesc.keepInitialState = true;
                bufferDesc.debugName = "AvgLuminanceBuffer";
                bufferDesc.canHaveTypedViews = true;
                perViewData.avgLuminanceBufferGPU = device->createBuffer(bufferDesc);

                bufferDesc.canHaveUAVs = false;
                bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::Read;
                bufferDesc.debugName = "AvgLuminanceReadbackBuffer";
                for (int i = 0; i < PerViewData::cReadbackLag; i++)
                {
                    perViewData.avgLuminanceBufferReadback[i] = device->createBuffer(bufferDesc);
                    perViewData.avgLuminanceReadbackQuery[i] = device->createEventQuery();
                }
            }
#endif

            perViewData.mipMapPass = std::make_unique<MipMapGenPass>(m_device, shaderFactory, perViewData.luminanceTexture, MipMapGenPass::MODE_COLOR);
        }

		caustica::rhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = caustica::rhi::ShaderType::Pixel;
		layoutDesc.bindings = {
			caustica::rhi::BindingLayoutItem::Texture_SRV(0),
			caustica::rhi::BindingLayoutItem::Sampler(1)
		};
		m_LuminanceBindingLayout = m_device->createBindingLayout(layoutDesc);

		caustica::rhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.primType = caustica::rhi::PrimitiveType::TriangleStrip;
		pipelineDesc.VS = m_renderDevice.blit().fullscreenVS();
		pipelineDesc.PS = m_LuminanceShader;
		pipelineDesc.bindingLayouts = { m_LuminanceBindingLayout };

		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
		pipelineDesc.renderState.depthStencilState.stencilEnable = false;

		m_LuminancePso = m_device->createGraphicsPipeline(pipelineDesc, m_PerView[0].luminanceFrameBuffer->getFramebufferInfo());

#if TONEMAPPING_AUTOEXPOSURE_CPU
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(0),
            caustica::rhi::BindingLayoutItem::Sampler(0)
        };
        m_CaptureLumBindingLayout = m_device->createBindingLayout(layoutDesc);
        caustica::rhi::ComputePipelineDesc captureLumPSODesc;
        captureLumPSODesc.bindingLayouts = { m_CaptureLumBindingLayout };
        captureLumPSODesc.CS = m_CaptureLuminanceShader;
        m_CaptureLumPso = m_device->createComputePipeline(captureLumPSODesc);
#endif
    }

    {
        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(1),
            caustica::rhi::BindingLayoutItem::Sampler(0),
            caustica::rhi::BindingLayoutItem::Sampler(1),
            caustica::rhi::BindingLayoutItem::Texture_SRV(2),
            caustica::rhi::BindingLayoutItem::Sampler(2)
        };
        m_ToneMapBindingLayout = m_device->createBindingLayout(layoutDesc);

        caustica::rhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = caustica::rhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_renderDevice.blit().fullscreenVS();
        pipelineDesc.PS = m_ToneMapShader;
        pipelineDesc.bindingLayouts = { m_ToneMapBindingLayout};

        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.stencilEnable = false;

        m_ToneMapPso = m_device->createGraphicsPipeline(pipelineDesc, colorSampleFramebuffer->getFramebufferInfo());
    }
}

void ToneMappingPass::preRender(const ToneMappingParameters& params)
{
    setParameters(params);
    uploadCameraLut3D();
    updateExposureValue();
    updateWhiteBalanceTransform();
    updateColorTransform();
    m_FrameParamsSet = true;
}

bool ToneMappingPass::render(
    caustica::rhi::CommandList* commandList, 
    const caustica::ICompositeView& compositeView,
    caustica::rhi::Texture* sourceTexture,
    caustica::rhi::Buffer* constantsBuffer,
    bool enabled)
{
    assert( m_FrameParamsSet ); // forgot to call preRender before this?
    assert(constantsBuffer);
    m_FrameParamsSet = false;

    // Formerly set when AE closed the primary list mid-pass (removed in ADR 0002 S1).
    constexpr bool commandListWasClosed = false;

    for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        PerViewData& viewData = m_PerView[viewIndex];

        if (viewData.sourceTexture != sourceTexture)
        {
            // make sure that our cached binding sets represent the right source texture.
            viewData.luminanceBindingSet = nullptr;
            viewData.colorBindingSet = nullptr;
            viewData.sourceTexture = sourceTexture;
        }
    }

    if(m_AutoExposure) 
    {
		commandList->beginMarker("Luminance");
		for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
		{
            PerViewData & viewData = m_PerView[viewIndex];

            caustica::rhi::BindingSetHandle& bindingSet = viewData.luminanceBindingSet;
			if (!bindingSet)
			{
				caustica::rhi::BindingSetDesc bindingSetDesc;
				bindingSetDesc.bindings = {
					caustica::rhi::BindingSetItem::Texture_SRV(0, sourceTexture),
					caustica::rhi::BindingSetItem::Sampler(1, m_linearSampler)
				};
				bindingSet = m_device->createBindingSet(bindingSetDesc, m_LuminanceBindingLayout);
			}

			const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);

			caustica::rhi::GraphicsState state;
			state.pipeline = m_LuminancePso;
            state.framebuffer = viewData.luminanceFrameBuffer;
			state.bindings = { bindingSet };
            caustica::rhi::ViewportState viewportState;
            viewportState.addViewport(viewData.luminanceFrameBuffer->getFramebufferInfo().getViewport());
            viewportState.addScissorRect(caustica::rhi::Rect(viewData.luminanceFrameBuffer->getFramebufferInfo().getViewport()));
            state.viewport = viewportState;
           	commandList->setGraphicsState(state);

			caustica::rhi::DrawArguments args;
			args.instanceCount = 1;
			args.vertexCount = 4;
			commandList->draw(args);    

            generateMips(commandList, compositeView.getNumChildViews(ViewType::PLANAR));

#if TONEMAPPING_AUTOEXPOSURE_CPU
            {
                caustica::rhi::BindingSetDesc bindingSetDesc; bindingSetDesc.bindings = {
                        caustica::rhi::BindingSetItem::Texture_SRV(0, viewData.luminanceTexture),
                        caustica::rhi::BindingSetItem::TypedBuffer_UAV(0, viewData.avgLuminanceBufferGPU),
                        caustica::rhi::BindingSetItem::Sampler(0, (true) ? m_linearSampler : m_pointSampler) };
                caustica::rhi::BindingSetHandle cbindingSet = m_device->createBindingSet(bindingSetDesc, m_CaptureLumBindingLayout);
                caustica::rhi::ComputeState cstate;
                cstate.bindings = { cbindingSet };
                cstate.pipeline = m_CaptureLumPso;
                commandList->setComputeState(cstate);
                commandList->dispatch(1, 1);

                // Record into a free slot. The queue fence is attached only
                // after this frame is submitted; an in-flight slot is never
                // mapped or overwritten.
                if (viewData.avgLuminanceRecordedThisFrame < 0)
                {
                    for (int offset = 0; offset < PerViewData::cReadbackLag; ++offset)
                    {
                        const int slot =
                            (viewData.avgLuminanceNextWrite + offset) % PerViewData::cReadbackLag;
                        if (viewData.avgLuminanceReadbackPending[slot]
                            || !viewData.avgLuminanceReadbackQuery[slot])
                            continue;

                        commandList->copyBuffer(
                            viewData.avgLuminanceBufferReadback[slot],
                            0,
                            viewData.avgLuminanceBufferGPU,
                            0,
                            viewData.avgLuminanceBufferReadback[slot]->getDesc().byteSize);
                        viewData.avgLuminanceRecordedThisFrame = slot;
                        viewData.avgLuminanceNextWrite = (slot + 1) % PerViewData::cReadbackLag;
                        break;
                    }
                }
            }
#endif
		}
		commandList->endMarker();
    }

    commandList->beginMarker("ToneMapping");
    for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
    {
		caustica::rhi::BindingSetHandle& bindingSet = m_PerView[viewIndex].colorBindingSet;
		if (!bindingSet)
		{
			caustica::rhi::BindingSetDesc bindingSetDesc;
			bindingSetDesc.bindings = {
				caustica::rhi::BindingSetItem::ConstantBuffer(0, constantsBuffer),
				caustica::rhi::BindingSetItem::Texture_SRV(0, sourceTexture),                       //Color texture
				caustica::rhi::BindingSetItem::Texture_SRV(1, m_PerView[viewIndex].luminanceTexture),                    //Luminance Texture
				caustica::rhi::BindingSetItem::Sampler(0, m_linearSampler),    //Luminance sampler
				caustica::rhi::BindingSetItem::Sampler(1, m_pointSampler),     //Color sampler
                caustica::rhi::BindingSetItem::Texture_SRV(2, m_cameraLut3DTexture),
                caustica::rhi::BindingSetItem::Sampler(2, m_cameraLutSampler)
			};
			bindingSet = m_device->createBindingSet(bindingSetDesc, m_ToneMapBindingLayout);
		}
        const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);

        caustica::rhi::GraphicsState state;
        state.pipeline = m_ToneMapPso;
        state.framebuffer = m_FramebufferFactory->getFramebuffer(*view);
        state.bindings = { bindingSet };
        state.viewport = toRhi(view->getViewportState());

        ToneMappingConstants toneMappingConsts = {};
        toneMappingConsts.whiteScale = m_WhiteScale;
        toneMappingConsts.whiteMaxLuminance = m_WhiteMaxLuminance;
        toneMappingConsts.clamped = m_Clamped;
        toneMappingConsts.toneMapOperator = (uint32_t)m_ToneMapOperator;
        toneMappingConsts.autoExposure = m_AutoExposure;
#if TONEMAPPING_AUTOEXPOSURE_CPU
        toneMappingConsts.avgLuminance = m_PerView[viewIndex].avgLuminanceLastCaptured;
#else
        toneMappingConsts.avgLuminance = 0; // unused
#endif
        if(m_AutoExposure)
        {
            toneMappingConsts.autoExposureLumValueMin = std::exp2f( m_ExposureValueMin );
            toneMappingConsts.autoExposureLumValueMax = std::exp2f( m_ExposureValueMax );
        }
        else
        {
            toneMappingConsts.autoExposureLumValueMin = std::exp2f( -16.0 );
            toneMappingConsts.autoExposureLumValueMax = std::exp2f( 16.0 );
        }

        // Copy 3x3 to 3x4 Is there a better way to do this?
       /* affine3 transform(m_ColorTransform, float3(0,0,0));
        affineToColumnMajor(transform, toneMappingConsts.colorTransform);*/ //incorrect output, why?
		toneMappingConsts.colorTransform = float3x4::identity();
		toneMappingConsts.colorTransform[0] = float4(m_ColorTransform.col(0), 0);
		toneMappingConsts.colorTransform[1] = float4(m_ColorTransform.col(1), 0);
		toneMappingConsts.colorTransform[2] = float4(m_ColorTransform.col(2), 0);
        toneMappingConsts.enabled = enabled;
        toneMappingConsts.cameraLutEnabled = m_CameraLutEnabled;
        toneMappingConsts.cameraLutDomainMin = m_CameraLutDomainMin;
        toneMappingConsts.cameraLutDomainMax = m_CameraLutDomainMax;
        toneMappingConsts.cameraLutSize = ToneMappingParameters::CameraLutSize;
        toneMappingConsts.cameraLutAfterToneMap = m_CameraLutAfterToneMap;
        toneMappingConsts.cameraLutIs3D = m_CameraLutIs3D;
        for (uint32_t i = 0; i < ToneMappingParameters::CameraLutSize; ++i)
            toneMappingConsts.cameraLut[i] = float4(m_CameraLut[i], 0.0f);
        commandList->writeBuffer(constantsBuffer, &toneMappingConsts, sizeof(ToneMappingConstants));

        commandList->setGraphicsState(state);

        caustica::rhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = 4;
        commandList->draw(args);
    }
    commandList->endMarker();
    return commandListWasClosed;
}

void ToneMappingPass::registerGraphPass(
    caustica::rg::GraphBuilder& graph,
    caustica::rg::TextureHandle sourceColor,
    caustica::rg::TextureHandle outputLdrColor,
    caustica::PlanarView compositeView,
    bool enabled,
    bool* outCommandListWasClosed)
{
    const caustica::rg::BufferHandle constantsBuffer = graph.importBuffer(
        m_ToneMappingCB,
        caustica::rg::BufferAccess::ConstantBuffer);

    graph.addPass(
        "ToneMapping",
        [sourceColor, outputLdrColor, constantsBuffer](caustica::rg::PassBuilder& setup) {
            setup.read(sourceColor, caustica::rg::TextureAccess::ShaderResource);
            setup.write(constantsBuffer, caustica::rg::BufferAccess::ConstantBuffer);
            setup.write(outputLdrColor, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, sourceColor, constantsBuffer, compositeView = std::move(compositeView),
         enabled, outCommandListWasClosed](caustica::rg::RenderPassContext& ctx) {
            const bool closed = render(
                ctx.commandList(),
                compositeView,
                ctx.texture(sourceColor),
                ctx.buffer(constantsBuffer),
                enabled);
            if (outCommandListWasClosed)
                *outCommandListWasClosed = closed;
        },
        caustica::rg::PassOptions{
            .sideEffect = true,
            // ADR 0002 S1: AE no longer closes the primary list mid-pass.
            .serialOnPrimary = false,
        });
}

void ToneMappingPass::advanceFrame(float frameTime)
{
    m_FrameTime = frameTime;
    m_FrameParamsSet = false;
}

void ToneMappingPass::onFrameSubmitted()
{
#if TONEMAPPING_AUTOEXPOSURE_CPU
    for (PerViewData& viewData : m_PerView)
    {
        // Consume completed older samples before fencing the copy submitted by
        // this frame. This keeps AE asynchronous while making CPU reads valid.
        for (int slot = 0; slot < PerViewData::cReadbackLag; ++slot)
        {
            if (!viewData.avgLuminanceReadbackPending[slot])
                continue;

            caustica::rhi::EventQuery* query = viewData.avgLuminanceReadbackQuery[slot];
            if (!query || !m_device->pollEventQuery(query))
                continue;

            void* pData = m_device->mapBuffer(
                viewData.avgLuminanceBufferReadback[slot],
                caustica::rhi::CpuAccessMode::Read);
            assert(pData);
            const float capturedLogLuminance = *static_cast<float*>(pData);
            viewData.avgLuminanceLastCaptured = std::exp2f(capturedLogLuminance);
            m_device->unmapBuffer(viewData.avgLuminanceBufferReadback[slot]);
            m_device->resetEventQuery(query);
            viewData.avgLuminanceReadbackPending[slot] = false;
        }

        const int submittedSlot = viewData.avgLuminanceRecordedThisFrame;
        if (submittedSlot < 0)
            continue;

        caustica::rhi::EventQuery* query =
            viewData.avgLuminanceReadbackQuery[submittedSlot];
        if (query)
        {
            m_device->resetEventQuery(query);
            m_device->setEventQuery(query, caustica::rhi::CommandQueue::Graphics);
            viewData.avgLuminanceReadbackPending[submittedSlot] = true;
        }
        viewData.avgLuminanceRecordedThisFrame = -1;
    }
#endif
}


static const float g_minLogLuminance = -10; // TODO: figure out how to set these properly
static const float g_maxLogLuminamce = 4;

void ToneMappingPass::setParameters(const ToneMappingParameters& params)
{
    m_ExposureMode = params.exposureMode;
    m_ToneMapOperator = params.toneMapOperator;
    m_AutoExposure = params.autoExposure;
    m_ExposureCompensation = params.exposureCompensation;
	m_ExposureValue = params.exposureValue;
	m_FilmSpeed = params.filmSpeed;
	m_FNumber = params.fNumber;
	m_Shutter = params.shutter;
	m_WhiteBalance = params.whiteBalance;
	m_WhitePoint = params.whitePoint;
	m_WhiteMaxLuminance = params.whiteMaxLuminance;
	m_WhiteScale = params.whiteScale;
    m_Clamped = params.clamped;
    m_ExposureValueMin = params.exposureValueMin;
    m_ExposureValueMax = params.exposureValueMax;
    m_CameraLutEnabled = params.cameraLutEnabled;
    m_CameraLutAfterToneMap = params.cameraLutAfterToneMap;
    m_CameraLutDomainMin = params.cameraLutDomainMin;
    m_CameraLutDomainMax = params.cameraLutDomainMax;
    m_CameraLut = params.cameraLut;
    m_CameraLutIs3D = params.cameraLutIs3D;
    m_CameraLut3DSize = params.cameraLut3DSize;
    m_CameraLutRevision = params.cameraLutRevision;
    m_CameraLut3D = params.cameraLut3D;
}

void ToneMappingPass::uploadCameraLut3D()
{
    // Do not create or submit GPU work for the default (disabled) path. Apart
    // from avoiding unnecessary startup work, this ensures the fixed 3D SRV
    // binding always uses the RenderDevice-owned fallback texture until a
    // custom 3D LUT is explicitly enabled.
    if (!m_CameraLutEnabled || !m_CameraLutIs3D || !m_CameraLut3D || m_CameraLut3DSize < 2)
        return;

    if (m_UploadedCameraLutRevision == m_CameraLutRevision && m_cameraLut3DTexture)
        return;

    const uint32_t size = m_CameraLut3DSize;
    const std::vector<float4>* values = m_CameraLut3D.get();

    caustica::rhi::TextureDesc desc;
    desc.dimension = caustica::rhi::TextureDimension::Texture3D;
    desc.width = size;
    desc.height = size;
    desc.depth = size;
    desc.format = caustica::rhi::Format::RGBA32_FLOAT;
    desc.debugName = "CameraLut3D";
    m_cameraLut3DTexture = m_device->createTexture(desc);

    caustica::rhi::CommandListHandle commandList = m_device->createCommandList();
    if (!commandList || !commandList->open())
        throw std::runtime_error("Failed to create a command list for CameraLut3D upload");

    commandList->beginTrackingTextureState(
        m_cameraLut3DTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);
    commandList->writeTexture(
        m_cameraLut3DTexture, 0, 0, values->data(),
        size_t(size) * sizeof(float4), size_t(size) * size * sizeof(float4));
    commandList->setTextureState(
        m_cameraLut3DTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(
        m_cameraLut3DTexture, caustica::rhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    commandList->close();
    m_device->executeCommandList(commandList);
    m_UploadedCameraLutRevision = m_CameraLutRevision;
    for (PerViewData& viewData : m_PerView)
        viewData.colorBindingSet = nullptr;
}


void ToneMappingPass::updateWhiteBalanceTransform()
{
	//Calculate color transform for the current white point. 
	m_WhiteBalanceTransform = m_WhiteBalance ?
		calculateWhiteBalanceTransformRGB_Rec709(m_WhitePoint) :
		dm::float3x3::identity();

	//Calculate source illuminant, i.e. the color that transform to a pure white (1, 1, 1) output at the current color settings.
	m_SourceWhite = inverse(m_WhiteBalanceTransform) * float3(1, 1, 1);
}
    
void ToneMappingPass::updateExposureValue()
{
	const float kShutterMin = 0.001f;        // Min reciprocal shutter time
	const float kShutterMax = 10000.f;       // Max reciprocal shutter time
	const float kFNumberMin = 0.1f;          // Minimum fNumber, > 0 to avoid numerical issues (i.e., non-physical values are allowed)
	const float kFNumberMax = 100.f;       

	// EV is ultimately derived from shutter and fNumber; set its range based on that of its inputs
	const float kExposureValueMin = std::log2(kShutterMin * kFNumberMin * kFNumberMin);
    const float kExposureValueMax = std::log2(kShutterMax * kFNumberMax * kFNumberMax);
    m_ExposureValue = clamp(m_ExposureValue, kExposureValueMin, kExposureValueMax);

    switch (m_ExposureMode)
    {
	case ExposureMode::AperturePriority:
		// Set shutter based on EV and aperture.
		m_Shutter = std::pow(2.f, m_ExposureValue) / (m_FNumber * m_FNumber);
		m_Shutter = clamp(m_Shutter, kShutterMin, kShutterMax);
        break;
    case ExposureMode::ShutterPriority:
		// Set aperture based on EV and shutter.
		m_FNumber = std::sqrt(std::pow(2.f, m_ExposureValue) / m_Shutter);
		m_FNumber = clamp(m_FNumber, kFNumberMin, kFNumberMax);
    }
}

void ToneMappingPass::updateColorTransform()
{
	//Exposure scale due to exposure compensation 
	float exposureScale = pow(2.f, m_ExposureCompensation);
	float manualExposureScale = 1.f;

	if (!m_AutoExposure)
	{
       	float normConstant = 1.f / 100.f;
		manualExposureScale = (normConstant * m_FilmSpeed) / (m_Shutter * m_FNumber * m_FNumber);
	}
	m_ColorTransform = m_WhiteBalanceTransform * exposureScale * manualExposureScale;
}

#if TONEMAPPING_AUTOEXPOSURE_CPU
float3 ToneMappingPass::getPreExposedGray(uint viewIndex) 
{ 
    assert(m_FrameParamsSet); // forgot to call preRender before this?
    assert(viewIndex < m_PerView.size()); 
    if (viewIndex >= m_PerView.size()) 
        return float3(0,0,0); 

    // should probably sync to be exact https://en.wikipedia.org/wiki/Middle_gray in the future but it does the trick for now
    float3 result = inverse(m_ColorTransform) * float3(0.18f, 0.18f, 0.18f);
    if (m_AutoExposure)
    {
        const float avgLum = std::max(m_PerView[viewIndex].avgLuminanceLastCaptured, 1e-6f);
        result /= float3((float)TONEMAPPING_EXPOSURE_KEY / avgLum);
    }
    return result;
}
#endif

void ToneMappingPass::generateMips(caustica::rhi::CommandList* commandList, uint32_t numberOfViews)
{
    for (uint32_t i = 0; i < numberOfViews; i++)
    {
        m_PerView[i].mipMapPass->dispatch(commandList);
    }
}
