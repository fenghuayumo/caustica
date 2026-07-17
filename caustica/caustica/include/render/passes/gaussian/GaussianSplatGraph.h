#pragma once

#include <math/vector.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <scene/SceneRenderData.h>

#include <shaders/SampleConstantBuffer.h>

#include <span>

namespace caustica::render
{

struct GaussianSplatBinding
{
    const GaussianSplatPass* splatPass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

struct GaussianSplatFrameInputs
{
    const PathTracerSettings& settings;
    int frameIndex = 0;
    int sampleIndex = 0;
    int temporalSampleIndex = 0;
    bool renderToOutputColor = false;
    dm::float2 displaySize = dm::float2(0.0f, 0.0f);
    dm::float3 shadowDirectionToLight = dm::float3(0.0f, 1.0f, 0.0f);
};

uint32_t resolveGaussianSplatShadowMode(const PathTracerSettings& settings);
uint32_t clampGaussianSplatSoftShadowSamples(int sampleCount);
uint32_t clampGaussianSplatEmissionProxyCount(int proxyCount);

bool isGaussianSplatEmissionEnabled(const PathTracerSettings& settings);
void fillGaussianSplatShadowConstants(
    SampleConstants& constants,
    const PathTracerSettings& settings,
    const GaussianSplatBinding& primaryBinding,
    uint32_t frameIndex);

bool needsStochasticGaussianSplatsBeforeAA(const PathTracerSettings& settings);
bool needsGaussianSplatsCompositePass(const PathTracerSettings& settings);
bool needsGaussianSplatStochasticAccumulate(const PathTracerSettings& settings);
bool needsGaussianSplatAccelBuild(const PathTracerSettings& settings);

GaussianSplatRenderSettings buildGaussianSplatRenderSettings(const GaussianSplatFrameInputs& inputs);

dm::float3 resolveGaussianSplatShadowDirection(std::span<const scene::LightRenderProxy> lights);

} // namespace caustica::render
