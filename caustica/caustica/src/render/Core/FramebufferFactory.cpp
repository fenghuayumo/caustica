#include <render/Core/FramebufferFactory.h>
#include <scene/View.h>

using namespace caustica;

nvrhi::IFramebuffer* FramebufferFactory::GetFramebuffer(const nvrhi::TextureSubresourceSet& subresources)
{
    nvrhi::FramebufferHandle& item = m_FramebufferCache[subresources];

    if (!item)
    {
        nvrhi::FramebufferDesc desc;
        for (auto renderTarget : RenderTargets)
            desc.addColorAttachment(renderTarget, subresources);

        if (DepthTarget)
            desc.setDepthAttachment(DepthTarget, subresources);

        if (ShadingRateSurface)
            desc.setShadingRateAttachment(ShadingRateSurface, subresources);

        item = m_Device->createFramebuffer(desc);
    }
    
    return item;
}

nvrhi::IFramebuffer* FramebufferFactory::GetFramebuffer(const IView& view)
{
    return GetFramebuffer(view.GetSubresources());
}

nvrhi::FramebufferInfo FramebufferFactory::GetFramebufferInfo()
{
    nvrhi::FramebufferInfo info;

    for (auto rt : RenderTargets)
    {
        info.addColorFormat(rt->getDesc().format);
    }

    if (DepthTarget)
        info.setDepthFormat(DepthTarget->getDesc().format);
    
    // Assume all textures have the same sample count
    if (!RenderTargets.empty())
    {
        info.setSampleCount(RenderTargets[0]->getDesc().sampleCount);
        info.setSampleQuality(RenderTargets[0]->getDesc().sampleQuality);
    }
    else if (DepthTarget)
    {
        info.setSampleCount(DepthTarget->getDesc().sampleCount);
        info.setSampleQuality(DepthTarget->getDesc().sampleQuality);
    }

    return info;
}
