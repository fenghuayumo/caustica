#include <render/rhi/ViewRhiConversion.h>

namespace caustica::render::rhi
{

namespace
{
    template<typename TDesc, typename TNvrhi>
    TDesc castEnum(TNvrhi value)
    {
        return static_cast<TDesc>(static_cast<uint8_t>(value));
    }
}

nvrhi::Viewport toNvrhi(const ViewportDesc& viewport)
{
    return nvrhi::Viewport(viewport.minX, viewport.maxX, viewport.minY, viewport.maxY, viewport.minZ, viewport.maxZ);
}

ViewportDesc fromNvrhi(const nvrhi::Viewport& viewport)
{
    return ViewportDesc(viewport.minX, viewport.maxX, viewport.minY, viewport.maxY, viewport.minZ, viewport.maxZ);
}

nvrhi::Rect toNvrhi(const ScissorDesc& scissor)
{
    return nvrhi::Rect(scissor.minX, scissor.maxX, scissor.minY, scissor.maxY);
}

ScissorDesc fromNvrhi(const nvrhi::Rect& scissor)
{
    return ScissorDesc(scissor.minX, scissor.maxX, scissor.minY, scissor.maxY);
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

ViewportStateDesc fromNvrhi(const nvrhi::ViewportState& state)
{
    ViewportStateDesc result;
    for (const nvrhi::Viewport& viewport : state.viewports)
        result.addViewport(fromNvrhi(viewport));
    for (const nvrhi::Rect& scissor : state.scissorRects)
        result.addScissorRect(fromNvrhi(scissor));
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

TextureSubresourceDesc fromNvrhi(const nvrhi::TextureSubresourceSet& subresources)
{
    return TextureSubresourceDesc(
        subresources.baseMipLevel,
        subresources.numMipLevels,
        subresources.baseArraySlice,
        subresources.numArraySlices);
}

nvrhi::VariableRateShadingState toNvrhi(const VariableRateShadingDesc& state)
{
    nvrhi::VariableRateShadingState result;
    result.enabled = state.enabled;
    result.shadingRate = castEnum<nvrhi::VariableShadingRate>(state.shadingRate);
    result.pipelinePrimitiveCombiner = castEnum<nvrhi::ShadingRateCombiner>(state.pipelinePrimitiveCombiner);
    result.imageCombiner = castEnum<nvrhi::ShadingRateCombiner>(state.imageCombiner);
    return result;
}

VariableRateShadingDesc fromNvrhi(const nvrhi::VariableRateShadingState& state)
{
    VariableRateShadingDesc result;
    result.enabled = state.enabled;
    result.shadingRate = castEnum<VariableShadingRateDesc>(state.shadingRate);
    result.pipelinePrimitiveCombiner = castEnum<ShadingRateCombinerDesc>(state.pipelinePrimitiveCombiner);
    result.imageCombiner = castEnum<ShadingRateCombinerDesc>(state.imageCombiner);
    return result;
}

} // namespace caustica::render::rhi
