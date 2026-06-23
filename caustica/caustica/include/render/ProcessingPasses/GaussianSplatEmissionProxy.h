#pragma once

#include <math/math.h>

struct GaussianSplatEmissionProxy
{
    caustica::math::float3 center = caustica::math::float3(0.0f);
    float radius = 0.0f;
    caustica::math::float3 radiance = caustica::math::float3(0.0f);
    float weight = 0.0f;
};
