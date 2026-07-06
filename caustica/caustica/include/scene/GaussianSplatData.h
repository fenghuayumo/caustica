#pragma once

#include <math/vector.h>

#include <cstdint>

namespace caustica
{

// GPU-compatible splat vertex layout (matches shaders/SampleConstantBuffer.h).
struct GaussianSplatData
{
    math::float4 centerOpacity;
    math::float4 covariance0;
    math::float4 covariance1;
    math::float4 color;
};

constexpr uint32_t kGaussianSplatShFloat4Count = 12;

static_assert(sizeof(GaussianSplatData) == sizeof(math::float4) * 4);

} // namespace caustica
