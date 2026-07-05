#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace caustica
{

struct ViewportDesc
{
    float minX = 0.f;
    float maxX = 0.f;
    float minY = 0.f;
    float maxY = 0.f;
    float minZ = 0.f;
    float maxZ = 1.f;

    ViewportDesc() = default;

    ViewportDesc(float width, float height)
        : minX(0.f)
        , maxX(width)
        , minY(0.f)
        , maxY(height)
        , minZ(0.f)
        , maxZ(1.f)
    {
    }

    ViewportDesc(float _minX, float _maxX, float _minY, float _maxY, float _minZ, float _maxZ)
        : minX(_minX)
        , maxX(_maxX)
        , minY(_minY)
        , maxY(_maxY)
        , minZ(_minZ)
        , maxZ(_maxZ)
    {
    }

    [[nodiscard]] float width() const { return maxX - minX; }
    [[nodiscard]] float height() const { return maxY - minY; }
};

struct ScissorDesc
{
    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;

    ScissorDesc() = default;

    ScissorDesc(int width, int height)
        : minX(0)
        , maxX(width)
        , minY(0)
        , maxY(height)
    {
    }

    ScissorDesc(int _minX, int _maxX, int _minY, int _maxY)
        : minX(_minX)
        , maxX(_maxX)
        , minY(_minY)
        , maxY(_maxY)
    {
    }

    explicit ScissorDesc(const ViewportDesc& viewport)
        : minX(int(std::floor(viewport.minX)))
        , maxX(int(std::ceil(viewport.maxX)))
        , minY(int(std::floor(viewport.minY)))
        , maxY(int(std::ceil(viewport.maxY)))
    {
    }

    [[nodiscard]] int width() const { return maxX - minX; }
    [[nodiscard]] int height() const { return maxY - minY; }
};

struct TextureSubresourceDesc
{
    static constexpr int32_t AllMipLevels = -1;
    static constexpr int32_t AllArraySlices = -1;

    int32_t baseMipLevel = 0;
    int32_t numMipLevels = 1;
    int32_t baseArraySlice = 0;
    int32_t numArraySlices = 1;

    TextureSubresourceDesc() = default;

    TextureSubresourceDesc(int32_t _baseMipLevel, int32_t _numMipLevels, int32_t _baseArraySlice, int32_t _numArraySlices)
        : baseMipLevel(_baseMipLevel)
        , numMipLevels(_numMipLevels)
        , baseArraySlice(_baseArraySlice)
        , numArraySlices(_numArraySlices)
    {
    }
};

enum class VariableShadingRateDesc : uint8_t
{
    Rate1x1,
    Rate1x2,
    Rate2x1,
    Rate2x2,
    Rate2x4,
    Rate4x2,
    Rate4x4,
};

enum class ShadingRateCombinerDesc : uint8_t
{
    Passthrough,
    Override,
    Min,
    Max,
    ApplyRelative,
};

struct VariableRateShadingDesc
{
    bool enabled = false;
    VariableShadingRateDesc shadingRate = VariableShadingRateDesc::Rate1x1;
    ShadingRateCombinerDesc pipelinePrimitiveCombiner = ShadingRateCombinerDesc::Passthrough;
    ShadingRateCombinerDesc imageCombiner = ShadingRateCombinerDesc::Passthrough;
};

struct ViewportStateDesc
{
    std::vector<ViewportDesc> viewports;
    std::vector<ScissorDesc> scissorRects;

    ViewportStateDesc& addViewport(const ViewportDesc& viewport)
    {
        viewports.push_back(viewport);
        return *this;
    }

    ViewportStateDesc& addScissorRect(const ScissorDesc& scissor)
    {
        scissorRects.push_back(scissor);
        return *this;
    }

    ViewportStateDesc& addViewportAndScissorRect(const ViewportDesc& viewport)
    {
        return addViewport(viewport).addScissorRect(ScissorDesc(viewport));
    }
};

} // namespace caustica
