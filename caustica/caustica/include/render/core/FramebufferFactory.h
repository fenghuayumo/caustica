#pragma once

#include <rhi/rhi.h>
#include <vector>
#include <unordered_map>

namespace caustica
{
    class IView;

    class FramebufferFactory
    {
    private:
        caustica::rhi::DeviceHandle m_device;
        std::unordered_map<caustica::rhi::TextureSubresourceSet, caustica::rhi::FramebufferHandle> m_framebufferCache;

    public:
        FramebufferFactory(caustica::rhi::Device* device) : m_device(device) {}
        virtual ~FramebufferFactory() = default;

        std::vector<caustica::rhi::TextureHandle> renderTargets;
        caustica::rhi::TextureHandle depthTarget;
        caustica::rhi::TextureHandle shadingRateSurface;

        virtual caustica::rhi::Framebuffer* getFramebuffer(const caustica::rhi::TextureSubresourceSet& subresources);
        caustica::rhi::Framebuffer* getFramebuffer(const IView& view);
        caustica::rhi::FramebufferInfo getFramebufferInfo();
    };
}