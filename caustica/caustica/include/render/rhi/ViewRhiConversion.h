#pragma once

#include <scene/ViewDesc.h>
#include <rhi/nvrhi.h>

namespace caustica::render::rhi
{

[[nodiscard]] nvrhi::Viewport toNvrhi(const ViewportDesc& viewport);
[[nodiscard]] nvrhi::Rect toNvrhi(const ScissorDesc& scissor);
[[nodiscard]] nvrhi::ViewportState toNvrhi(const ViewportStateDesc& state);
[[nodiscard]] nvrhi::TextureSubresourceSet toNvrhi(const TextureSubresourceDesc& subresources);

} // namespace caustica::render::rhi
