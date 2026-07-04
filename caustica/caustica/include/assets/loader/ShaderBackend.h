#pragma once

#include <cstdint>

namespace nvrhi
{
enum class GraphicsAPI : uint8_t;
}

namespace caustica::shader
{

enum class Backend : uint8_t
{
    D3D11,
    D3D12,
    Vulkan,
};

[[nodiscard]] Backend fromNvrhiGraphicsApi(nvrhi::GraphicsAPI api);
[[nodiscard]] nvrhi::GraphicsAPI toNvrhiGraphicsApi(Backend backend);
[[nodiscard]] const char* backendToken(Backend backend);

} // namespace caustica::shader
