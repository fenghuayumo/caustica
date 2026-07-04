#include <render/Passes/Geometry/BloomPass.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/RenderPassConstants.h>
#include <rhi/RenderDevice.h>
#include <scene/View.h>
#include <utility>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/bloom_ps.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/bloom_ps.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/bloom_ps.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/bloom_cb.h>

using namespace caustica;
using namespace caustica::render;

BloomPass::BloomPass(
    nvrhi::IDevice* device,
    const std::shared_ptr<ShaderFactory>& shaderFactory,
    caustica::rhi::RenderDevice& renderDevice,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView)
    : m_renderDevice(renderDevice)
    , m_FramebufferFactory(std::move(framebufferFactory))
    , m_Device(device)
    , m_BindingCache(device)
{
    m_BloomBlurPixelShader = shaderFactory->CreateAutoShader("engine/passes/bloom_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_bloom_ps), nullptr, nvrhi::ShaderType::Pixel);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(BloomConstants);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.debugName = "BloomConstantsH";
    constantBufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
    m_BloomHBlurCB = device->createBuffer(constantBufferDesc);
    constantBufferDesc.debugName = "BloomConstantsV";
    m_BloomVBlurCB = device->createBuffer(constantBufferDesc);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0)
    };
    m_BloomBlurBindingLayout = device->createBindingLayout(layoutDesc);

    m_PerViewData.resize(compositeView.GetNumChildViews(ViewType::PLANAR));
    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        
        nvrhi::IFramebuffer* sampleFramebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        PerViewData& perViewData = m_PerViewData[viewIndex];

        nvrhi::Rect viewExtent = view->GetViewExtent();
        int viewportWidth = viewExtent.maxX - viewExtent.minX;
        int viewportHeight = viewExtent.maxY - viewExtent.minY;

        // temporary textures for downscaling
        nvrhi::TextureDesc downscaleTextureDesc;
        downscaleTextureDesc.format = sampleFramebuffer->getFramebufferInfo().colorFormats[0];
        downscaleTextureDesc.width = (uint32_t)ceil(viewportWidth / 2.f);
        downscaleTextureDesc.height = (uint32_t)ceil(viewportHeight / 2.f);
        downscaleTextureDesc.mipLevels = 1;
        downscaleTextureDesc.isRenderTarget = true;
        downscaleTextureDesc.debugName = "bloom src mip1";
        downscaleTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        downscaleTextureDesc.keepInitialState = true;
        perViewData.textureDownscale1 = m_Device->createTexture(downscaleTextureDesc);
        perViewData.framebufferDownscale1 = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(perViewData.textureDownscale1));

        downscaleTextureDesc.debugName = "bloom src mip2";
        downscaleTextureDesc.width = (uint32_t)ceil(downscaleTextureDesc.width / 2.f);
        downscaleTextureDesc.height = (uint32_t)ceil(downscaleTextureDesc.height / 2.f);
        perViewData.textureDownscale2 = m_Device->createTexture(downscaleTextureDesc);
        perViewData.framebufferDownscale2 = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(perViewData.textureDownscale2));

        // intermediate textures for accumulating blur
        nvrhi::TextureDesc intermediateTextureDesc2;
        intermediateTextureDesc2.format = downscaleTextureDesc.format;
        intermediateTextureDesc2.width = downscaleTextureDesc.width;
        intermediateTextureDesc2.height = downscaleTextureDesc.height;
        intermediateTextureDesc2.mipLevels = 1;
        intermediateTextureDesc2.isRenderTarget = true;

        intermediateTextureDesc2.debugName = "bloom accumulation pass1";
        intermediateTextureDesc2.initialState = nvrhi::ResourceStates::ShaderResource;
        intermediateTextureDesc2.keepInitialState = true;
        perViewData.texturePass1Blur = m_Device->createTexture(intermediateTextureDesc2);
        perViewData.framebufferPass1Blur = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(perViewData.texturePass1Blur));

        intermediateTextureDesc2.debugName = "bloom accumulation pass2";
        perViewData.texturePass2Blur = m_Device->createTexture(intermediateTextureDesc2);
        perViewData.framebufferPass2Blur = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(perViewData.texturePass2Blur));

        nvrhi::GraphicsPipelineDesc graphicsPipelineDesc;
        graphicsPipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        graphicsPipelineDesc.VS = m_renderDevice.blit().fullscreenVS();
        graphicsPipelineDesc.PS = m_BloomBlurPixelShader;
        graphicsPipelineDesc.bindingLayouts = { m_BloomBlurBindingLayout };
        graphicsPipelineDesc.renderState.rasterState.setCullNone();
        graphicsPipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        graphicsPipelineDesc.renderState.depthStencilState.stencilEnable = false;
        perViewData.bloomBlurPso = device->createGraphicsPipeline(graphicsPipelineDesc,
            perViewData.framebufferPass1Blur->getFramebufferInfo());
        
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_BloomHBlurCB),
            nvrhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().linearClamp()),
            nvrhi::BindingSetItem::Texture_SRV(0, perViewData.textureDownscale2)
        };
        perViewData.bloomBlurBindingSetPass1 = m_Device->createBindingSet(bindingSetDesc, m_BloomBlurBindingLayout);

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_BloomVBlurCB),
            nvrhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().linearClamp()),
            nvrhi::BindingSetItem::Texture_SRV(0, perViewData.texturePass1Blur)
        };
        perViewData.bloomBlurBindingSetPass2 = m_Device->createBindingSet(bindingSetDesc, m_BloomBlurBindingLayout);
    }
}

void BloomPass::Render(
	nvrhi::ICommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
	nvrhi::ITexture* sourceDestTexture,
    float sigmaInPixels,
    float blendFactor)
{
    float effectiveSigma = clamp(sigmaInPixels * 0.25f, 1.f, 100.f);

    commandList->beginMarker("Bloom");

    nvrhi::DrawArguments fullscreenquadargs;
    fullscreenquadargs.instanceCount = 1;
    fullscreenquadargs.vertexCount = 4;

    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        nvrhi::IFramebuffer* framebuffer = framebufferFactory->GetFramebuffer(*view);
        PerViewData& perViewData = m_PerViewData[viewIndex];

        nvrhi::ViewportState viewportState = view->GetViewportState();
        const nvrhi::Rect& scissorRect = viewportState.scissorRects[0];
        const nvrhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();

        // downscale
        {
            commandList->beginMarker("Downscale");

            dm::box2 uvSrcRect = box2(
                float2(
                    float(scissorRect.minX) / (float)fbinfo.width,
                    float(scissorRect.minY) / (float)fbinfo.height),
                float2(
                    float(scissorRect.maxX) / (float)fbinfo.width,
                    float(scissorRect.maxY) / (float)fbinfo.height)
            );

            // half-scale down

            rhi::BlitParameters blitParams1;
            blitParams1.targetFramebuffer = perViewData.framebufferDownscale1;
            blitParams1.sourceTexture = sourceDestTexture;
            blitParams1.sourceBox = uvSrcRect;
            m_renderDevice.blit().blitTexture(commandList, blitParams1, &m_BindingCache);

            // half-scale again down to quarter-scale

            rhi::BlitParameters blitParams2;
            blitParams2.targetFramebuffer = perViewData.framebufferDownscale2;
            blitParams2.sourceTexture = perViewData.textureDownscale1;
            m_renderDevice.blit().blitTexture(commandList, blitParams2, &m_BindingCache);

            commandList->endMarker(); // "Downscale"
        }

        // apply blur
        {
            commandList->beginMarker("Blur");
            nvrhi::Viewport viewport;

            nvrhi::GraphicsState state;
            state.pipeline = perViewData.bloomBlurPso;
            viewport = nvrhi::Viewport(float(perViewData.texturePass1Blur->getDesc().width), float(perViewData.texturePass1Blur->getDesc().height));
            state.viewport.addViewport(viewport);
            state.viewport.addScissorRect(nvrhi::Rect(viewport));
            state.framebuffer = perViewData.framebufferPass1Blur;
            state.bindings = { perViewData.bloomBlurBindingSetPass1 };

            BloomConstants bloomHorizonal = {};
            bloomHorizonal.pixstep.x = 1.f / perViewData.texturePass1Blur->getDesc().width;
            bloomHorizonal.pixstep.y = 0.f;
            bloomHorizonal.argumentScale = -1.f / (2 * effectiveSigma * effectiveSigma);
            bloomHorizonal.normalizationScale = 1.f / (sqrtf(2 * PI_f) * effectiveSigma);
            bloomHorizonal.numSamples = ::round(effectiveSigma * 4.f);
            BloomConstants bloomVertical = bloomHorizonal;
            bloomVertical.pixstep.x = 0.f;
            bloomVertical.pixstep.y = 1.f / perViewData.texturePass1Blur->getDesc().height;
            commandList->writeBuffer(m_BloomHBlurCB, &bloomHorizonal, sizeof(bloomHorizonal));
            commandList->writeBuffer(m_BloomVBlurCB, &bloomVertical, sizeof(bloomVertical));

            commandList->setGraphicsState(state);
            commandList->draw(fullscreenquadargs); // blur to m_TexturePass1Blur or m_TexturePass3Blur

            viewport = nvrhi::Viewport(float(perViewData.texturePass2Blur->getDesc().width), float(perViewData.texturePass2Blur->getDesc().height));
            state.viewport.viewports[0] = viewport;
            state.viewport.scissorRects[0] = nvrhi::Rect(viewport);
            state.framebuffer = perViewData.framebufferPass2Blur;
            state.bindings = { perViewData.bloomBlurBindingSetPass2 };

            commandList->setGraphicsState(state);
            commandList->draw(fullscreenquadargs); // blur to m_TexturePass2Blur

            commandList->endMarker(); // "Blur"
        }

        // composite
        {
            commandList->beginMarker("Apply");

            rhi::BlitParameters blitParams3;
            blitParams3.targetFramebuffer = framebuffer;
            blitParams3.targetViewport = viewportState.viewports[0];
            blitParams3.sourceTexture = perViewData.texturePass2Blur;
            blitParams3.blendState.setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::ConstantColor)
                .setDestBlend(nvrhi::BlendFactor::InvConstantColor)
                .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                .setDestBlendAlpha(nvrhi::BlendFactor::One);
            blitParams3.blendConstantColor = nvrhi::Color(blendFactor);
            m_renderDevice.blit().blitTexture(commandList, blitParams3, &m_BindingCache);

            commandList->endMarker(); // "Apply"
        }
    }

    commandList->endMarker(); // "Bloom"
}

void BloomPass::registerGraphPass(
    caustica::rg::GraphBuilder& graph,
    caustica::rg::TextureHandle processedOutputColor,
    const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
    const caustica::ICompositeView& compositeView,
    float sigmaInPixels,
    float blendFactor,
    bool enabled)
{
    graph.addPass(
        "Bloom",
        [&](caustica::rg::PassBuilder& setup) {
            setup.read(processedOutputColor, caustica::rg::TextureAccess::ShaderResource);
            setup.write(processedOutputColor, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, processedOutputColor, framebufferFactory, &compositeView, sigmaInPixels, blendFactor](caustica::rg::RenderPassContext& ctx) {
            Render(
                ctx.commandList(),
                framebufferFactory,
                compositeView,
                ctx.texture(processedOutputColor),
                sigmaInPixels,
                blendFactor);
        },
        enabled);
}
