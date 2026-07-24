#pragma once

#include <scene/ViewDesc.h>
#include <rhi/rhi.h>

namespace caustica
{

[[nodiscard]] caustica::rhi::Viewport toRhi(const ViewportDesc& viewport);
[[nodiscard]] caustica::rhi::Rect toRhi(const ScissorDesc& scissor);
[[nodiscard]] caustica::rhi::ViewportState toRhi(const ViewportStateDesc& state);
[[nodiscard]] caustica::rhi::TextureSubresourceSet toRhi(const TextureSubresourceDesc& subresources);

} // namespace caustica
