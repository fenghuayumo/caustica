#pragma once

#include <render/graph/GraphBuilder.h>
#include <rhi/nvrhi.h>
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

        nvrhi::DeviceHandle m_device;

        struct PerViewData
        {
            nvrhi::GraphicsPipelineHandle bloomBlurPso;
            nvrhi::Format                 psoColorFormat = nvrhi::Format::UNKNOWN;
        };

        std::vector<PerViewData> m_PerViewData;
        nvrhi::BufferHandle m_BloomHBlurCB;
        nvrhi::BufferHandle m_BloomVBlurCB;
        nvrhi::ShaderHandle m_BloomBlurPixelShader;
        nvrhi::BindingLayoutHandle m_BloomBlurBindingLayout;
        nvrhi::BindingLayoutHandle m_BloomApplyBindingLayout;

        void ensureBlurPso(uint32_t viewIndex, nvrhi::IFramebuffer* framebuffer);

        void renderInternal(
            nvrhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            nvrhi::ITexture* sourceDestTexture,
            nvrhi::ITexture* textureDownscale1,
            nvrhi::ITexture* textureDownscale2,
            nvrhi::ITexture* texturePass1Blur,
            nvrhi::ITexture* texturePass2Blur,
            float sigmaInPixels,
            float blendFactor);

    public:
        BloomPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ICompositeView& compositeView);

        void Render(
            nvrhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            nvrhi::ITexture* sourceDestTexture,
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
