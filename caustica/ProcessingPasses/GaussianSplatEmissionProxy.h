/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <donut/core/math/math.h>

struct GaussianSplatEmissionProxy
{
    donut::math::float3 center = donut::math::float3(0.0f);
    float radius = 0.0f;
    donut::math::float3 radiance = donut::math::float3(0.0f);
    float weight = 0.0f;
};
