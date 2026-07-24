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
        const caustica::rhi::IFramebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
        const caustica::rhi::Format nativeFormat = framebuffer->getFramebufferInfo().colorFormats[0];
        return rg::fromNativeFormat(nativeFormat);
    }
}

BloomPass::BloomPass(
    caustica::rhi::IDevice* device,
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

void BloomPass::ensureBlurPso(uint32_t viewIndex, caustica::rhi::IFramebuffer* framebuffer)
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

void BloomPass::renderInternal(
    caustica::rhi::ICommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
    caustica::rhi::ITexture* sourceDestTexture,
    caustica::rhi::ITexture* textureDownscale1,
    caustica::rhi::ITexture* textureDownscale2,
    caustica::rhi::ITexture* texturePass1Blur,
    caustica::rhi::ITexture* texturePass2Blur,
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
        caustica::rhi::IFramebuffer* framebuffer = framebufferFactory->getFramebuffer(*view);
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
    caustica::rhi::ICommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
    caustica::rhi::ITexture* sourceDestTexture,
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
    const caustica::ICompositeView& compositeView,
    float sigmaInPixels,
    float blendFactor,
    bool enabled)
{
    struct BloomGraphPassData
    {
        rg::TextureHandle outputColor{};
        rg::TextureHandle downscale1{};
        rg::TextureHandle downscale2{};
        rg::TextureHandle pass1Blur{};
        rg::TextureHandle pass2Blur{};
    };

    auto passData = std::make_shared<BloomGraphPassData>();
    passData->outputColor = processedOutputColor;

    graph.addPass(
        "Bloom",
        [passData, framebufferFactory, &compositeView](caustica::rg::PassBuilder& setup) {
            const IView* view = compositeView.getChildView(ViewType::PLANAR, 0);
            uint32_t mip1W = 0;
            uint32_t mip1H = 0;
            uint32_t mip2W = 0;
            uint32_t mip2H = 0;
            const rg::Format colorFormat = bloomColorFormat(framebufferFactory, compositeView);
            computeBloomMipSizes(view, mip1W, mip1H, mip2W, mip2H);

            passData->downscale1 = setup.createTexture(makeBloomMipDesc(mip1W, mip1H, colorFormat, "bloom src mip1"));
            passData->downscale2 = setup.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom src mip2"));
            passData->pass1Blur = setup.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom accumulation pass1"));
            passData->pass2Blur = setup.createTexture(makeBloomMipDesc(mip2W, mip2H, colorFormat, "bloom accumulation pass2"));

            setup.read(passData->outputColor, caustica::rg::TextureAccess::ShaderResource);
            setup.write(passData->downscale1, caustica::rg::TextureAccess::RenderTarget);
            setup.read(passData->downscale1, caustica::rg::TextureAccess::ShaderResource);
            setup.write(passData->downscale2, caustica::rg::TextureAccess::RenderTarget);
            setup.read(passData->downscale2, caustica::rg::TextureAccess::ShaderResource);
            setup.write(passData->pass1Blur, caustica::rg::TextureAccess::RenderTarget);
            setup.read(passData->pass1Blur, caustica::rg::TextureAccess::ShaderResource);
            setup.write(passData->pass2Blur, caustica::rg::TextureAccess::RenderTarget);
            setup.read(passData->pass2Blur, caustica::rg::TextureAccess::ShaderResource);
            setup.write(passData->outputColor, caustica::rg::TextureAccess::RenderTarget);
        },
        [this, passData, framebufferFactory, &compositeView, sigmaInPixels, blendFactor](caustica::rg::RenderPassContext& ctx) {
            renderInternal(
                ctx.commandList(),
                framebufferFactory,
                compositeView,
                ctx.texture(passData->outputColor),
                ctx.texture(passData->downscale1),
                ctx.texture(passData->downscale2),
                ctx.texture(passData->pass1Blur),
                ctx.texture(passData->pass2Blur),
                sigmaInPixels,
                blendFactor);
        },
        caustica::rg::PassOptions{ .enabled = enabled });
}
