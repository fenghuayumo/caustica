#include <render/passes/gaussian/GaussianSplatGeometry.h>

#include <algorithm>
#include <cmath>

namespace caustica::render
{

namespace
{
    constexpr float kGaussianSplatKernelMinResponse = 0.0113f;
    constexpr float kIcosahedronInvPhi = 0.61803398875f;

    float gaussianKernelScale(float density, float kernelMinResponse, uint32_t kernelDegree, bool adaptiveClamping)
    {
        const float responseModulation = adaptiveClamping ? std::max(density, 1e-6f) : 1.0f;
        const float minResponse = std::min(kernelMinResponse / responseModulation, 0.97f);

        if (kernelDegree == 0)
            return std::abs((minResponse - 1.0f) / -0.329630334487f);

        const float b = std::max(float(kernelDegree), 1.0f);
        const float a = -4.5f / std::pow(3.0f, b);
        return std::pow(std::log(minResponse) / a, 1.0f / b);
    }

    float srgbToLinearChannel(float srgb)
    {
        srgb = std::max(srgb, 0.0f);
        return srgb <= 0.04045f
            ? srgb / 12.92f
            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }
}

const std::array<caustica::math::float3, 12> kGaussianSplatUnitIcosahedronVertices = {
    caustica::math::float3(-kIcosahedronInvPhi,  1.0f, 0.0f),
    caustica::math::float3( kIcosahedronInvPhi,  1.0f, 0.0f),
    caustica::math::float3(-kIcosahedronInvPhi, -1.0f, 0.0f),
    caustica::math::float3( kIcosahedronInvPhi, -1.0f, 0.0f),
    caustica::math::float3(0.0f, -kIcosahedronInvPhi,  1.0f),
    caustica::math::float3(0.0f,  kIcosahedronInvPhi,  1.0f),
    caustica::math::float3(0.0f, -kIcosahedronInvPhi, -1.0f),
    caustica::math::float3(0.0f,  kIcosahedronInvPhi, -1.0f),
    caustica::math::float3( 1.0f, 0.0f, -kIcosahedronInvPhi),
    caustica::math::float3( 1.0f, 0.0f,  kIcosahedronInvPhi),
    caustica::math::float3(-1.0f, 0.0f, -kIcosahedronInvPhi),
    caustica::math::float3(-1.0f, 0.0f,  kIcosahedronInvPhi)
};

const std::array<uint32_t, 60> kGaussianSplatUnitIcosahedronIndices = {
    0, 11, 5,
    0, 5, 1,
    0, 1, 7,
    0, 7, 10,
    0, 10, 11,
    1, 5, 9,
    5, 11, 4,
    11, 10, 2,
    10, 7, 6,
    7, 1, 8,
    3, 9, 4,
    3, 4, 2,
    3, 2, 6,
    3, 6, 8,
    3, 8, 9,
    4, 9, 5,
    2, 4, 11,
    6, 2, 10,
    8, 6, 7,
    9, 8, 1
};

caustica::math::float3 gaussianAabbExtent(
    const caustica::GaussianSplatData& splat,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    const caustica::math::float3 variance = caustica::math::float3(
        std::max(splat.covariance0.x, 1e-8f),
        std::max(splat.covariance0.w, 1e-8f),
        std::max(splat.covariance1.y, 1e-8f));
    const float kernelScale = gaussianKernelScale(
        splat.centerOpacity.w,
        kGaussianSplatKernelMinResponse,
        kernelDegree,
        adaptiveClamp);
    return caustica::math::float3(
        std::sqrt(variance.x),
        std::sqrt(variance.y),
        std::sqrt(variance.z)) * (std::max(splatScale, 1e-4f) * std::max(kernelScale, 1e-3f));
}

caustica::rhi::rt::GeometryAABB gaussianAabbFromSplat(
    const caustica::GaussianSplatData& splat,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    const caustica::math::float3 center = splat.centerOpacity.xyz();
    const caustica::math::float3 extent = gaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp);

    caustica::rhi::rt::GeometryAABB aabb = {};
    aabb.minX = center.x - extent.x;
    aabb.minY = center.y - extent.y;
    aabb.minZ = center.z - extent.z;
    aabb.maxX = center.x + extent.x;
    aabb.maxY = center.y + extent.y;
    aabb.maxZ = center.z + extent.z;
    return aabb;
}

std::vector<caustica::rhi::rt::GeometryAABB> buildGaussianAabbs(
    const std::vector<caustica::GaussianSplatData>& splats,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    std::vector<caustica::rhi::rt::GeometryAABB> aabbs;
    aabbs.reserve(splats.size());

    for (const caustica::GaussianSplatData& splat : splats)
        aabbs.push_back(gaussianAabbFromSplat(splat, splatScale, kernelDegree, adaptiveClamp));

    return aabbs;
}

void fillScaleTranslateTransform(
    caustica::rhi::rt::AffineTransform& transform,
    const caustica::math::float3& center,
    const caustica::math::float3& extent)
{
    transform[0] = extent.x; transform[1] = 0.0f;     transform[2] = 0.0f;     transform[3] = center.x;
    transform[4] = 0.0f;     transform[5] = extent.y; transform[6] = 0.0f;     transform[7] = center.y;
    transform[8] = 0.0f;     transform[9] = 0.0f;     transform[10] = extent.z; transform[11] = center.z;
}

caustica::math::float3 srgbToLinear(const caustica::math::float3& srgb)
{
    return caustica::math::float3(
        srgbToLinearChannel(srgb.x),
        srgbToLinearChannel(srgb.y),
        srgbToLinearChannel(srgb.z));
}

float luminance(const caustica::math::float3& color)
{
    return dot(color, caustica::math::float3(0.2126f, 0.7152f, 0.0722f));
}

} // namespace caustica::render
