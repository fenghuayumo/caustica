#include <render/Core/FramebufferFactory.h>
#include <scene/View.h>

using namespace caustica;

nvrhi::IFramebuffer* FramebufferFactory::getFramebuffer(const nvrhi::TextureSubresourceSet& subresources)
{
    nvrhi::FramebufferHandle& item = m_framebufferCache[subresources];

    if (!item)
    {
        nvrhi::FramebufferDesc desc;
        for (auto renderTarget : renderTargets)
            desc.addColorAttachment(renderTarget, subresources);

        if (depthTarget)
            desc.setDepthAttachment(depthTarget, subresources);

        if (shadingRateSurface)
            desc.setShadingRateAttachment(shadingRateSurface, subresources);

        item = m_device->createFramebuffer(desc);
    }
    
    return item;
}

nvrhi::IFramebuffer* FramebufferFactory::getFramebuffer(const IView& view)
{
    return getFramebuffer(view.getSubresources());
}

nvrhi::FramebufferInfo FramebufferFactory::getFramebufferInfo()
{
    nvrhi::FramebufferInfo info;

    for (auto rt : renderTargets)
    {
        info.addColorFormat(rt->getDesc().format);
    }

    if (depthTarget)
        info.setDepthFormat(depthTarget->getDesc().format);
    
    // Assume all textures have the same sample count
    if (!renderTargets.empty())
    {
        info.setSampleCount(renderTargets[0]->getDesc().sampleCount);
        info.setSampleQuality(renderTargets[0]->getDesc().sampleQuality);
    }
    else if (depthTarget)
    {
        info.setSampleCount(depthTarget->getDesc().sampleCount);
        info.setSampleQuality(depthTarget->getDesc().sampleQuality);
    }

    return info;
}
