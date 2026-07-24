#include <render/core/FramebufferFactory.h>
#include <scene/View.h>
#include <backend/ViewRhiConversion.h>

using namespace caustica;

caustica::rhi::IFramebuffer* FramebufferFactory::getFramebuffer(const caustica::rhi::TextureSubresourceSet& subresources)
{
    caustica::rhi::FramebufferHandle& item = m_framebufferCache[subresources];

    if (!item)
    {
        caustica::rhi::FramebufferDesc desc;
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

caustica::rhi::IFramebuffer* FramebufferFactory::getFramebuffer(const IView& view)
{
    return getFramebuffer(caustica::toRhi(view.getSubresources()));
}

caustica::rhi::FramebufferInfo FramebufferFactory::getFramebufferInfo()
{
    caustica::rhi::FramebufferInfo info;

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
