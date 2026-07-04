#pragma once

#include <rhi/Format.h>

#include <cstdint>
#include <string>

namespace caustica::rhi
{

struct TextureDesc
{
    std::string name;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    Format format = Format::RGBA8_UNORM;
    bool isRenderTarget = false;
    bool isUAV = false;
    bool isTypeless = false;
};

struct BufferDesc
{
    std::string name;
    uint64_t byteSize = 0;
    bool isConstantBuffer = false;
    bool isStructuredBuffer = false;
    uint32_t structuredStride = 0;
};

// Opaque GPU resource handles for render-graph and pass code.
struct TextureHandle
{
    uint32_t index = UINT32_MAX;

    [[nodiscard]] bool isValid() const { return index != UINT32_MAX; }
};

struct BufferHandle
{
    uint32_t index = UINT32_MAX;

    [[nodiscard]] bool isValid() const { return index != UINT32_MAX; }
};

} // namespace caustica::rhi
