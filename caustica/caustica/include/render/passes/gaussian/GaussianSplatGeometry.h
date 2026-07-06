#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <scene/GaussianSplatData.h>

#include <array>
#include <vector>

namespace caustica::render
{

caustica::math::float3 gaussianAabbExtent(
    const caustica::GaussianSplatData& splat,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp);

nvrhi::rt::GeometryAABB gaussianAabbFromSplat(
    const caustica::GaussianSplatData& splat,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp);

std::vector<nvrhi::rt::GeometryAABB> buildGaussianAabbs(
    const std::vector<caustica::GaussianSplatData>& splats,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp);

void fillScaleTranslateTransform(
    nvrhi::rt::AffineTransform& transform,
    const caustica::math::float3& center,
    const caustica::math::float3& extent);

extern const std::array<caustica::math::float3, 12> kGaussianSplatUnitIcosahedronVertices;
extern const std::array<uint32_t, 60> kGaussianSplatUnitIcosahedronIndices;

caustica::math::float3 srgbToLinear(const caustica::math::float3& srgb);
float luminance(const caustica::math::float3& color);

} // namespace caustica::render
