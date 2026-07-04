#pragma once

#include <render/Core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <unordered_map>

#include <rhi/RenderDevice.h>

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
        caustica::rhi::RenderDevice& m_renderDevice;
        std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;

        nvrhi::DeviceHandle m_Device;

        struct PerViewData
        {
            nvrhi::GraphicsPipelineHandle bloomBlurPso;

            nvrhi::TextureHandle textureDownscale1;
            nvrhi::FramebufferHandle framebufferDownscale1;
            nvrhi::TextureHandle textureDownscale2;
            nvrhi::FramebufferHandle framebufferDownscale2;

            nvrhi::TextureHandle texturePass1Blur;
            nvrhi::FramebufferHandle framebufferPass1Blur;
            nvrhi::TextureHandle texturePass2Blur;
            nvrhi::FramebufferHandle framebufferPass2Blur;

            nvrhi::BindingSetHandle bloomBlurBindingSetPass1;
            nvrhi::BindingSetHandle bloomBlurBindingSetPass2;
            nvrhi::BindingSetHandle bloomBlurBindingSetPass3;
            nvrhi::BindingSetHandle blitFromDownscale1BindingSet;
            nvrhi::BindingSetHandle compositeBlitBindingSet;
        };

        std::vector<PerViewData> m_PerViewData;
        nvrhi::BufferHandle m_BloomHBlurCB;
        nvrhi::BufferHandle m_BloomVBlurCB;
        nvrhi::ShaderHandle m_BloomBlurPixelShader;
        nvrhi::BindingLayoutHandle m_BloomBlurBindingLayout;
        nvrhi::BindingLayoutHandle m_BloomApplyBindingLayout;

        caustica::BindingCache m_BindingCache;

    public:
        BloomPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            caustica::rhi::RenderDevice& renderDevice,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ICompositeView& compositeView);

        void Render(
            nvrhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            nvrhi::ITexture* sourceDestTexture,
            float sigmaInPixels,
            float blendFactor);
    };
}
