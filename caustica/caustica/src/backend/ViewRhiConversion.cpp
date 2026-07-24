#include <backend/ViewRhiConversion.h>

namespace caustica
{

caustica::rhi::Viewport toRhi(const ViewportDesc& viewport)
{
    return caustica::rhi::Viewport(viewport.minX, viewport.maxX, viewport.minY, viewport.maxY, viewport.minZ, viewport.maxZ);
}

caustica::rhi::Rect toRhi(const ScissorDesc& scissor)
{
    return caustica::rhi::Rect(scissor.minX, scissor.maxX, scissor.minY, scissor.maxY);
}

caustica::rhi::ViewportState toRhi(const ViewportStateDesc& state)
{
    caustica::rhi::ViewportState result;
    for (const ViewportDesc& viewport : state.viewports)
        result.addViewport(toRhi(viewport));
    for (const ScissorDesc& scissor : state.scissorRects)
        result.addScissorRect(toRhi(scissor));
    return result;
}

caustica::rhi::TextureSubresourceSet toRhi(const TextureSubresourceDesc& subresources)
{
    return caustica::rhi::TextureSubresourceSet(
        subresources.baseMipLevel,
        subresources.numMipLevels,
        subresources.baseArraySlice,
        subresources.numArraySlices);
}

} // namespace caustica
