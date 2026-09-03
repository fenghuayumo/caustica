#include <render/passes/geometry/BloomPass.h>
#include <render/core/FramebufferFactory.h>
#include <backend/ViewRhiConversion.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <render/graph/GpuTypes.h>
#include <render/core/RenderDevice.h>
#include <scene/View.h>
#include <utility>

#include <cmath>
#include <memory>

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

namespace
{
    rg::TextureDesc makeBloomMipDesc(uint32_t width, uint32_t height, rg::Format format, const char* name)
    {
        rg::TextureDesc desc{};
        desc.name = name;
        desc.width = width;
        desc.height = height;
        desc.mipLevels = 1;
        desc.format = format;
        desc.isRenderTarget = true;
        return desc;
    }

    void computeBloomMipSizes(const IView* view,
        uint32_t& mip1Width, uint32_t& mip1Height, uint32_t& mip2Width, uint32_t& mip2Height)
    {
        const ScissorDesc viewExtent = view->getViewExtent();
        const int viewportWidth = viewExtent.maxX - viewExtent.minX;
        const int viewportHeight = viewExtent.maxY - viewExtent.minY;

        mip1Width = static_cast<uint32_t>(std::ceil(viewportWidth / 2.f));
        mip1Height = static_cast<uint32_t>(std::ceil(viewportHeight / 2.f));
        mip2Width = static_cast<uint32_t>(std::ceil(mip1Width / 2.f));
        mip2Height = static_cast<uint32_t>(std::ceil(mip1Height / 2.f));
    }

    rg::Format bloomColorFormat(const std::shared_ptr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView)
    {
        const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
        const caustica::rhi::Framebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
        const caustica::rhi::Format nativeFormat = framebuffer->getFramebufferInfo().colorFormats[0];
        return rg::fromNativeFormat(nativeFormat);
    }
}

BloomPass::BloomPass(
    caustica::rhi::Device* device,
    const std::shared_ptr<ShaderFactory>& shaderFactory,
    caustica::render::RenderDevice& renderDevice,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView)
    : m_renderDevice(renderDevice)
    , m_FramebufferFactory(std::move(framebufferFactory))
    , m_device(device)
{
    m_BloomBlurPixelShader = shaderFactory->createAutoShader("engine/passes/bloom_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_bloom_ps), nullptr, caustica::rhi::ShaderType::Pixel);

    caustica::rhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(BloomConstants);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.debugName = "BloomConstantsH";
    constantBufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
    m_BloomHBlurCB = device->createBuffer(constantBufferDesc);
    constantBufferDesc.debugName = "BloomConstantsV";
    m_BloomVBlurCB = device->createBuffer(constantBufferDesc);

    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::Sampler(0),
        caustica::rhi::BindingLayoutItem::Texture_SRV(0)
    };
    m_BloomBlurBindingLayout = device->createBindingLayout(layoutDesc);

    m_PerViewData.resize(compositeView.getNumChildViews(ViewType::PLANAR));
}

void BloomPass::ensureBlurPso(uint32_t viewIndex, caustica::rhi::Framebuffer* framebuffer)
{
    PerViewData& perViewData = m_PerViewData[viewIndex];
    const caustica::rhi::Format colorFormat = framebuffer->getFramebufferInfo().colorFormats[0];
    if (perViewData.bloomBlurPso && perViewData.psoColorFormat == colorFormat)
        return;

    caustica::rhi::GraphicsPipelineDesc graphicsPipelineDesc;
    graphicsPipelineDesc.primType = caustica::rhi::PrimitiveType::TriangleStrip;
    graphicsPipelineDesc.VS = m_renderDevice.blit().fullscreenVS();
    graphicsPipelineDesc.PS = m_BloomBlurPixelShader;
    graphicsPipelineDesc.bindingLayouts = { m_BloomBlurBindingLayout };
    graphicsPipelineDesc.renderState.rasterState.setCullNone();
    graphicsPipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    graphicsPipelineDesc.renderState.depthStencilState.stencilEnable = false;
    perViewData.bloomBlurPso = m_device->createGraphicsPipeline(graphicsPipelineDesc, framebuffer->getFramebufferInfo());
    perViewData.psoColorFormat = colorFormat;
}

void BloomPass::executeDownscale1(
    caustica::rhi::CommandList* commandList,
    const ICompositeView& compositeView,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    caustica::rhi::Texture* source,
    caustica::rhi::Texture* dest)
{
    const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
    caustica::rhi::Framebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
    const caustica::rhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();
    const caustica::rhi::ViewportState viewportState = toRhi(view->getViewportState());
    const caustica::rhi::Rect& scissorRect = viewportState.scissorRects[0];
    const caustica::rhi::FramebufferHandle destFb = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(dest));

    const dm::box2 uvSrcRect = box2(
        float2(
            float(scissorRect.minX) / float(fbinfo.width),
            float(scissorRect.minY) / float(fbinfo.height)),
        float2(
            float(scissorRect.maxX) / float(fbinfo.width),
            float(scissorRect.maxY) / float(fbinfo.height)));

    caustica::render::BlitParameters blitParams;
    blitParams.targetFramebuffer = destFb;
    blitParams.sourceTexture = source;
    blitParams.sourceBox = uvSrcRect;
    m_renderDevice.blit().blitTexture(commandList, blitParams, nullptr);
}

void BloomPass::executeDownscale2(
    caustica::rhi::CommandList* commandList,
    caustica::rhi::Texture* source,
    caustica::rhi::Texture* dest)
{
    const caustica::rhi::FramebufferHandle destFb = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(dest));
    caustica::render::BlitParameters blitParams;
    blitParams.targetFramebuffer = destFb;
    blitParams.sourceTexture = source;
    m_renderDevice.blit().blitTexture(commandList, blitParams, nullptr);
}

void BloomPass::executeBlur(
    caustica::rhi::CommandList* commandList,
    const ICompositeView& compositeView,
    caustica::rhi::Texture* source,
    caustica::rhi::Texture* dest,
    caustica::rhi::Buffer* constants,
    bool horizontal,
    float sigmaInPixels)
{
    const float effectiveSigma = clamp(sigmaInPixels * 0.25f, 1.f, 100.f);
    const caustica::rhi::FramebufferHandle destFb = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(dest));
    ensureBlurPso(0, destFb);

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, constants),
        caustica::rhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().linearClamp()),
        caustica::rhi::BindingSetItem::Texture_SRV(0, source),
    };
    const caustica::rhi::BindingSetHandle bindingSet =
        m_device->createBindingSet(bindingSetDesc, m_BloomBlurBindingLayout);

    BloomConstants bloom = {};
    bloom.pixstep.x = horizontal ? 1.f / dest->getDesc().width : 0.f;
    bloom.pixstep.y = horizontal ? 0.f : 1.f / dest->getDesc().height;
    bloom.argumentScale = -1.f / (2 * effectiveSigma * effectiveSigma);
    bloom.normalizationScale = 1.f / (sqrtf(2 * PI_f) * effectiveSigma);
    bloom.numSamples = ::round(effectiveSigma * 4.f);
    commandList->writeBuffer(constants, &bloom, sizeof(bloom));

    caustica::rhi::GraphicsState state;
    state.pipeline = m_PerViewData[0].bloomBlurPso;
    const caustica::rhi::Viewport viewport(
        float(dest->getDesc().width),
        float(dest->getDesc().height));
    state.viewport.addViewport(viewport);
    state.viewport.addScissorRect(caustica::rhi::Rect(viewport));
    state.framebuffer = destFb;
    state.bindings = { bindingSet };
    commandList->setGraphicsState(state);

    caustica::rhi::DrawArguments fullscreenquadargs;
    fullscreenquadargs.instanceCount = 1;
    fullscreenquadargs.vertexCount = 4;
    commandList->draw(fullscreenquadargs);
    (void)compositeView;
}

void BloomPass::executeComposite(
    caustica::rhi::CommandList* commandList,
    const ICompositeView& compositeView,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    caustica::rhi::Texture* source,
    float blendFactor)
{
    const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
    caustica::rhi::Framebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
    const caustica::rhi::ViewportState viewportState = toRhi(view->getViewportState());

    caustica::render::BlitParameters blitParams;
    blitParams.targetFramebuffer = framebuffer;
    blitParams.targetViewport = viewportState.viewports[0];
    blitParams.sourceTexture = source;
    blitParams.blendState.setBlendEnable(true)
        .setSrcBlend(caustica::rhi::BlendFactor::ConstantColor)
        .setDestBlend(caustica::rhi::BlendFactor::InvConstantColor)
        .setSrcBlendAlpha(caustica::rhi::BlendFactor::Zero)
        .setDestBlendAlpha(caustica::rhi::BlendFactor::One);
    blitParams.blendConstantColor = caustica::rhi::Color(blendFactor);
    m_renderDevice.blit().blitTexture(commandList, blitParams, nullptr);
}

void BloomPass::renderInternal(
    caustica::rhi::CommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
    caustica::rhi::Texture* sourceDestTexture,
    caustica::rhi::Texture* textureDownscale1,
    caustica::rhi::Texture* textureDownscale2,
    caustica::rhi::Texture* texturePass1Blur,
    caustica::rhi::Texture* texturePass2Blur,
    float sigmaInPixels,
    float blendFactor)
{
    const float effectiveSigma = clamp(sigmaInPixels * 0.25f, 1.f, 100.f);

    commandList->beginMarker("Bloom");

    caustica::rhi::DrawArguments fullscreenquadargs;
    fullscreenquadargs.instanceCount = 1;
    fullscreenquadargs.vertexCount = 4;

    const caustica::rhi::FramebufferHandle framebufferDownscale1 = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(textureDownscale1));
    const caustica::rhi::FramebufferHandle framebufferDownscale2 = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(textureDownscale2));
    const caustica::rhi::FramebufferHandle framebufferPass1Blur = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(texturePass1Blur));
    const caustica::rhi::FramebufferHandle framebufferPass2Blur = m_device->createFramebuffer(
        caustica::rhi::FramebufferDesc().addColorAttachment(texturePass2Blur));

    for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);
        caustica::rhi::Framebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
        ensureBlurPso(viewIndex, framebufferPass1Blur);

        caustica::rhi::ViewportState viewportState = toRhi(view->getViewportState());
        const caustica::rhi::Rect& scissorRect = viewportState.scissorRects[0];
        const caustica::rhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();

        caustica::rhi::BindingSetDesc bindingSetDescPass1;
        bindingSetDescPass1.bindings = {
            caustica::rhi::BindingSetItem::ConstantBuffer(0, m_BloomHBlurCB),
            caustica::rhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().linearClamp()),
            caustica::rhi::BindingSetItem::Texture_SRV(0, textureDownscale2),
        };
        const caustica::rhi::BindingSetHandle bloomBlurBindingSetPass1 =
            m_device->createBindingSet(bindingSetDescPass1, m_BloomBlurBindingLayout);

        caustica::rhi::BindingSetDesc bindingSetDescPass2;
        bindingSetDescPass2.bindings = {
            caustica::rhi::BindingSetItem::ConstantBuffer(0, m_BloomVBlurCB),
            caustica::rhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().linearClamp()),
            caustica::rhi::BindingSetItem::Texture_SRV(0, texturePass1Blur),
        };
        const caustica::rhi::BindingSetHandle bloomBlurBindingSetPass2 =
            m_device->createBindingSet(bindingSetDescPass2, m_BloomBlurBindingLayout);

        {
            commandList->beginMarker("Downscale");

            const dm::box2 uvSrcRect = box2(
                float2(
                    float(scissorRect.minX) / (float)fbinfo.width,
                    float(scissorRect.minY) / (float)fbinfo.height),
                float2(
                    float(scissorRect.maxX) / (float)fbinfo.width,
                    float(scissorRect.maxY) / (float)fbinfo.height)
            );

            caustica::render::BlitParameters blitParams1;
            blitParams1.targetFramebuffer = framebufferDownscale1;
            blitParams1.sourceTexture = sourceDestTexture;
            blitParams1.sourceBox = uvSrcRect;
            m_renderDevice.blit().blitTexture(commandList, blitParams1, nullptr);

            caustica::render::BlitParameters blitParams2;
            blitParams2.targetFramebuffer = framebufferDownscale2;
            blitParams2.sourceTexture = textureDownscale1;
            m_renderDevice.blit().blitTexture(commandList, blitParams2, nullptr);

            commandList->endMarker();
        }

        {
            commandList->beginMarker("Blur");
            caustica::rhi::Viewport viewport;

            caustica::rhi::GraphicsState state;
            state.pipeline = m_PerViewData[viewIndex].bloomBlurPso;
            viewport = caustica::rhi::Viewport(float(texturePass1Blur->getDesc().width), float(texturePass1Blur->getDesc().height));
            state.viewport.addViewport(viewport);
            state.viewport.addScissorRect(caustica::rhi::Rect(viewport));
            state.framebuffer = framebufferPass1Blur;
            state.bindings = { bloomBlurBindingSetPass1 };

            BloomConstants bloomHorizonal = {};
            bloomHorizonal.pixstep.x = 1.f / texturePass1Blur->getDesc().width;
            bloomHorizonal.pixstep.y = 0.f;
            bloomHorizonal.argumentScale = -1.f / (2 * effectiveSigma * effectiveSigma);
            bloomHorizonal.normalizationScale = 1.f / (sqrtf(2 * PI_f) * effectiveSigma);
            bloomHorizonal.numSamples = ::round(effectiveSigma * 4.f);
            BloomConstants bloomVertical = bloomHorizonal;
            bloomVertical.pixstep.x = 0.f;
            bloomVertical.pixstep.y = 1.f / texturePass1Blur->getDesc().height;
            commandList->writeBuffer(m_BloomHBlurCB, &bloomHorizonal, sizeof(bloomHorizonal));
            commandList->writeBuffer(m_BloomVBlurCB, &bloomVertical, sizeof(bloomVertical));

            commandList->setGraphicsState(state);
            commandList->draw(fullscreenquadargs);

            viewport = caustica::rhi::Viewport(float(texturePass2Blur->getDesc().width), float(texturePass2Blur->getDesc().height));
            state.viewport.viewports[0] = viewport;
            state.viewport.scissorRects[0] = caustica::rhi::Rect(viewport);
            state.framebuffer = framebufferPass2Blur;
            state.bindings = { bloomBlurBindingSetPass2 };

            commandList->setGraphicsState(state);
            commandList->draw(fullscreenquadargs);

            commandList->endMarker();
        }

        {
            commandList->beginMarker("apply");

            caustica::render::BlitParameters blitParams3;
            blitParams3.targetFramebuffer = framebuffer;
            blitParams3.targetViewport = viewportState.viewports[0];
            blitParams3.sourceTexture = texturePass2Blur;
            blitParams3.blendState.setBlendEnable(true)
                .setSrcBlend(caustica::rhi::BlendFactor::ConstantColor)
                .setDestBlend(caustica::rhi::BlendFactor::InvConstantColor)
                .setSrcBlendAlpha(caustica::rhi::BlendFactor::Zero)
                .setDestBlendAlpha(caustica::rhi::BlendFactor::One);
            blitParams3.blendConstantColor = caustica::rhi::Color(blendFactor);
            m_renderDevice.blit().blitTexture(commandList, blitParams3, nullptr);

            commandList->endMarker();
        }
    }

    commandList->endMarker();
}

void BloomPass::render(
    caustica::rhi::CommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
    caustica::rhi::Texture* sourceDestTexture,
    float sigmaInPixels,
    float blendFactor)
{
    const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
    uint32_t mip1W = 0;
    uint32_t mip1H = 0;
    uint32_t mip2W = 0;
    uint32_t mip2H = 0;
    const rg::Format colorFormat = bloomColorFormat(framebufferFactory, compositeView);
    computeBloomMipSizes(view, mip1W, mip1H, mip2W, mip2H);

    caustica::rhi::TextureDesc nativeDesc;
    nativeDesc.format = rg::toNativeFormat(colorFormat);
    nativeDesc.width = mip1W;
    nativeDesc.height = mip1H;
    nativeDesc.mipLevels = 1;
    nativeDesc.isRenderTarget = true;
    nativeDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    nativeDesc.keepInitialState = true;

    nativeDesc.debugName = "bloom src mip1";
    const caustica::rhi::TextureHandle textureDownscale1 = m_device->createTexture(nativeDesc);
    nativeDesc.debugName = "bloom src mip2";
    nativeDesc.width = mip2W;
    nativeDesc.height = mip2H;
    const caustica::rhi::TextureHandle textureDownscale2 = m_device->createTexture(nativeDesc);
    nativeDesc.debugName = "bloom accumulation pass1";
    const caustica::rhi::TextureHandle texturePass1Blur = m_device->createTexture(nativeDesc);
    nativeDesc.debugName = "bloom accumulation pass2";
    const caustica::rhi::TextureHandle texturePass2Blur = m_device->createTexture(nativeDesc);

    renderInternal(
        commandList,
        framebufferFactory,
        compositeView,
        sourceDestTexture,
        textureDownscale1,
        textureDownscale2,
        texturePass1Blur,
        texturePass2Blur,
        sigmaInPixels,
        blendFactor);
}

void BloomPass::registerGraphPass(
    caustica::rg::GraphBuilder& graph,
    caustica::rg::TextureHandle processedOutputColor,
    const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
    caustica::PlanarView compositeView,
    float sigmaInPixels,
    float blendFactor,
    bool enabled)
{
    const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
    uint32_t mip1W = 0;
    uint32_t mip1H = 0;
    uint32_t mip2W = 0;
    uint32_t mip2H = 0;
    const rg::Format colorFormat = bloomColorFormat(framebufferFactory, compositeView);
    computeBloomMipSizes(view, mip1W, mip1H, mip2W, mip2H);

    const rg::TextureHandle downscale1 =
        graph.createTexture(makeBloomMipDesc(mip1W, mip1H, colorFormat, "bloom src mip1"));
    const rg::TextureHandle downscale2 =
        graph.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom src mip2"));
    const rg::TextureHandle pass1Blur =
        graph.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom accumulation pass1"));
    const rg::TextureHandle pass2Blur =
        graph.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom accumulation pass2"));
    const rg::TextureHandle outputColor = processedOutputColor;
    const caustica::rg::PassOptions enabledOption{ .enabled = enabled };

    graph.addPass(
        "BloomDownscale1",
        [outputColor, downscale1](caustica::rg::PassBuilder& setup) {
            setup.read(outputColor, caustica::rg::TextureAccess::ShaderResource);
            setup.write(downscale1, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, framebufferFactory, compositeView, outputColor, downscale1](caustica::rg::RenderPassContext& ctx) {
            executeDownscale1(
                ctx.commandList(),
                compositeView,
                framebufferFactory,
                ctx.texture(outputColor),
                ctx.texture(downscale1));
        },
        enabledOption);

    graph.addPass(
        "BloomDownscale2",
        [downscale1, downscale2](caustica::rg::PassBuilder& setup) {
            setup.read(downscale1, caustica::rg::TextureAccess::ShaderResource);
            setup.write(downscale2, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, downscale1, downscale2](caustica::rg::RenderPassContext& ctx) {
            executeDownscale2(ctx.commandList(), ctx.texture(downscale1), ctx.texture(downscale2));
        },
        enabledOption);

    graph.addPass(
        "BloomBlur1",
        [downscale2, pass1Blur](caustica::rg::PassBuilder& setup) {
            setup.read(downscale2, caustica::rg::TextureAccess::ShaderResource);
            setup.write(pass1Blur, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, compositeView, downscale2, pass1Blur, sigmaInPixels](caustica::rg::RenderPassContext& ctx) {
            executeBlur(
                ctx.commandList(),
                compositeView,
                ctx.texture(downscale2),
                ctx.texture(pass1Blur),
                m_BloomHBlurCB,
                true,
                sigmaInPixels);
        },
        enabledOption);

    graph.addPass(
        "BloomBlur2",
        [pass1Blur, pass2Blur](caustica::rg::PassBuilder& setup) {
            setup.read(pass1Blur, caustica::rg::TextureAccess::ShaderResource);
            setup.write(pass2Blur, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, compositeView, pass1Blur, pass2Blur, sigmaInPixels](caustica::rg::RenderPassContext& ctx) {
            executeBlur(
                ctx.commandList(),
                compositeView,
                ctx.texture(pass1Blur),
                ctx.texture(pass2Blur),
                m_BloomVBlurCB,
                false,
                sigmaInPixels);
        },
        enabledOption);

    graph.addPass(
        "BloomComposite",
        [pass2Blur, outputColor](caustica::rg::PassBuilder& setup) {
            setup.read(pass2Blur, caustica::rg::TextureAccess::ShaderResource);
            setup.write(outputColor, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, framebufferFactory, compositeView, pass2Blur, blendFactor](caustica::rg::RenderPassContext& ctx) {
            executeComposite(
                ctx.commandList(),
                compositeView,
                framebufferFactory,
                ctx.texture(pass2Blur),
                blendFactor);
        },
        enabledOption);
}
