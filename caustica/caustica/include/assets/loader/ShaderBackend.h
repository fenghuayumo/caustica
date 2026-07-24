#pragma once

#include <cstdint>
#include <rhi/rhi.h>

namespace caustica::shader
{

enum class Backend : uint8_t
{
    D3D11,
    D3D12,
    Vulkan,
};

[[nodiscard]] Backend fromRhiGraphicsApi(caustica::rhi::GraphicsAPI api);
[[nodiscard]] caustica::rhi::GraphicsAPI toRhiGraphicsApi(Backend backend);
[[nodiscard]] const char* backendToken(Backend backend);

} // namespace caustica::shader
