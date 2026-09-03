#pragma once

#include <render/graph/GraphBuilder.h>
#include <rhi/rhi.h>
#include <memory>
#include <unordered_map>

#include <render/core/RenderDevice.h>
#include <scene/View.h>

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ViewInfo;
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

        void ensureBlurPso(uint32_t viewIndex, caustica::rhi::Framebuffer* framebuffer);

        void renderInternal(
            caustica::rhi::CommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ViewInfo& compositeView,
            caustica::rhi::Texture* sourceDestTexture,
            caustica::rhi::Texture* textureDownscale1,
            caustica::rhi::Texture* textureDownscale2,
            caustica::rhi::Texture* texturePass1Blur,
            caustica::rhi::Texture* texturePass2Blur,
            float sigmaInPixels,
            float blendFactor);

        void executeDownscale1(
            caustica::rhi::CommandList* commandList,
            const caustica::ViewInfo& compositeView,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            caustica::rhi::Texture* source,
            caustica::rhi::Texture* dest);
        void executeDownscale2(
            caustica::rhi::CommandList* commandList,
            caustica::rhi::Texture* source,
            caustica::rhi::Texture* dest);
        void executeBlur(
            caustica::rhi::CommandList* commandList,
            const caustica::ViewInfo& compositeView,
            caustica::rhi::Texture* source,
            caustica::rhi::Texture* dest,
            caustica::rhi::Buffer* constants,
            bool horizontal,
            float sigmaInPixels);
        void executeComposite(
            caustica::rhi::CommandList* commandList,
            const caustica::ViewInfo& compositeView,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            caustica::rhi::Texture* source,
            float blendFactor);

    public:
        BloomPass(
            caustica::rhi::Device* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ViewInfo& compositeView);

        void render(
            caustica::rhi::CommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ViewInfo& compositeView,
            caustica::rhi::Texture* sourceDestTexture,
            float sigmaInPixels,
            float blendFactor);

        void registerGraphPass(
            caustica::rg::GraphBuilder& graph,
            caustica::rg::TextureHandle processedOutputColor,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            caustica::ViewInfo compositeView,
            float sigmaInPixels,
            float blendFactor,
            bool enabled);
    };
}
