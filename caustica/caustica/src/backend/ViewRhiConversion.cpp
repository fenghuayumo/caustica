#include <backend/ViewRhiConversion.h>

namespace caustica
{

nvrhi::Viewport toNvrhi(const ViewportDesc& viewport)
{
    return nvrhi::Viewport(viewport.minX, viewport.maxX, viewport.minY, viewport.maxY, viewport.minZ, viewport.maxZ);
}

nvrhi::Rect toNvrhi(const ScissorDesc& scissor)
{
    return nvrhi::Rect(scissor.minX, scissor.maxX, scissor.minY, scissor.maxY);
}

nvrhi::ViewportState toNvrhi(const ViewportStateDesc& state)
{
    nvrhi::ViewportState result;
    for (const ViewportDesc& viewport : state.viewports)
        result.addViewport(toNvrhi(viewport));
    for (const ScissorDesc& scissor : state.scissorRects)
        result.addScissorRect(toNvrhi(scissor));
    return result;
}

nvrhi::TextureSubresourceSet toNvrhi(const TextureSubresourceDesc& subresources)
{
    return nvrhi::TextureSubresourceSet(
        subresources.baseMipLevel,
        subresources.numMipLevels,
        subresources.baseArraySlice,
        subresources.numArraySlices);
}

} // namespace caustica
