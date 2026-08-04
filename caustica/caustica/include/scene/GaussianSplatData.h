#pragma once

#include <math/vector.h>

#include <cstdint>

namespace caustica
{

// GPU-compatible splat vertex layout (matches shaders/FrameConstantBuffer.h).
struct GaussianSplatData
{
    math::float4 centerOpacity;
    math::float4 covariance0;
    math::float4 covariance1;
    math::float4 color;
    math::float4 scale;     // xyz = exp(log-scale), w unused (needed by 3DGUT UT)
    math::float4 rotation;  // wxyz unit quaternion (INRIA / PLY convention)
};

constexpr uint32_t kGaussianSplatShFloat4Count = 12;

static_assert(sizeof(GaussianSplatData) == sizeof(math::float4) * 6);

} // namespace caustica
