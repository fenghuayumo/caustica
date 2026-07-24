#pragma once

#include <render/graph/GraphBuilder.h>
#include <rhi/rhi.h>
#include <memory>
#include <unordered_map>

#include <render/core/RenderDevice.h>

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
    class BloomPass
    {
    private:
        caustica::render::RenderDevice& m_renderDevice;
        std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;

        caustica::rhi::DeviceHandle m_device;

        struct PerViewData
        {
            caustica::rhi::GraphicsPipelineHandle bloomBlurPso;
            caustica::rhi::Format                 psoColorFormat = caustica::rhi::Format::UNKNOWN;
        };

        std::vector<PerViewData> m_PerViewData;
        caustica::rhi::BufferHandle m_BloomHBlurCB;
        caustica::rhi::BufferHandle m_BloomVBlurCB;
        caustica::rhi::ShaderHandle m_BloomBlurPixelShader;
        caustica::rhi::BindingLayoutHandle m_BloomBlurBindingLayout;
        caustica::rhi::BindingLayoutHandle m_BloomApplyBindingLayout;

        void ensureBlurPso(uint32_t viewIndex, caustica::rhi::IFramebuffer* framebuffer);

        void renderInternal(
            caustica::rhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            caustica::rhi::ITexture* sourceDestTexture,
            caustica::rhi::ITexture* textureDownscale1,
            caustica::rhi::ITexture* textureDownscale2,
            caustica::rhi::ITexture* texturePass1Blur,
            caustica::rhi::ITexture* texturePass2Blur,
            float sigmaInPixels,
            float blendFactor);

    public:
        BloomPass(
            caustica::rhi::IDevice* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ICompositeView& compositeView);

        void render(
            caustica::rhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            caustica::rhi::ITexture* sourceDestTexture,
            float sigmaInPixels,
            float blendFactor);

        void registerGraphPass(
            caustica::rg::GraphBuilder& graph,
            caustica::rg::TextureHandle processedOutputColor,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            float sigmaInPixels,
            float blendFactor,
            bool enabled);
    };
}
