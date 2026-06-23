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
        nvrhi::DeviceHandle m_Device;
        std::unordered_map<nvrhi::TextureSubresourceSet, nvrhi::FramebufferHandle> m_FramebufferCache;

    public:
        FramebufferFactory(nvrhi::IDevice* device) : m_Device(device) {}
        virtual ~FramebufferFactory() = default;

        std::vector<nvrhi::TextureHandle> RenderTargets;
        nvrhi::TextureHandle DepthTarget;
        nvrhi::TextureHandle ShadingRateSurface;

        virtual nvrhi::IFramebuffer* GetFramebuffer(const nvrhi::TextureSubresourceSet& subresources);
        nvrhi::IFramebuffer* GetFramebuffer(const IView& view);
        nvrhi::FramebufferInfo GetFramebufferInfo();
    };
}