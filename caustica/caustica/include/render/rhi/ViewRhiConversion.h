#pragma once

#include <scene/ViewDesc.h>
#include <rhi/nvrhi.h>

namespace caustica::render::rhi
{

[[nodiscard]] nvrhi::Viewport toNvrhi(const ViewportDesc& viewport);
[[nodiscard]] ViewportDesc fromNvrhi(const nvrhi::Viewport& viewport);

[[nodiscard]] nvrhi::Rect toNvrhi(const ScissorDesc& scissor);
[[nodiscard]] ScissorDesc fromNvrhi(const nvrhi::Rect& scissor);

[[nodiscard]] nvrhi::ViewportState toNvrhi(const ViewportStateDesc& state);
[[nodiscard]] ViewportStateDesc fromNvrhi(const nvrhi::ViewportState& state);

[[nodiscard]] nvrhi::TextureSubresourceSet toNvrhi(const TextureSubresourceDesc& subresources);
[[nodiscard]] TextureSubresourceDesc fromNvrhi(const nvrhi::TextureSubresourceSet& subresources);

[[nodiscard]] nvrhi::VariableRateShadingState toNvrhi(const VariableRateShadingDesc& state);
[[nodiscard]] VariableRateShadingDesc fromNvrhi(const nvrhi::VariableRateShadingState& state);

} // namespace caustica::render::rhi
