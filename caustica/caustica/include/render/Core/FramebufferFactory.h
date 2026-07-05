#pragma once

#include <rhi/nvrhi.h>
#include <vector>
#include <unordered_map>

namespace caustica
{
    class IView;

    class FramebufferFactory
    {
    private:
        nvrhi::DeviceHandle m_device;
        std::unordered_map<nvrhi::TextureSubresourceSet, nvrhi::FramebufferHandle> m_framebufferCache;

    public:
        FramebufferFactory(nvrhi::IDevice* device) : m_device(device) {}
        virtual ~FramebufferFactory() = default;

        std::vector<nvrhi::TextureHandle> renderTargets;
        nvrhi::TextureHandle depthTarget;
        nvrhi::TextureHandle shadingRateSurface;

        virtual nvrhi::IFramebuffer* getFramebuffer(const nvrhi::TextureSubresourceSet& subresources);
        nvrhi::IFramebuffer* getFramebuffer(const IView& view);
        nvrhi::FramebufferInfo getFramebufferInfo();
    };
}