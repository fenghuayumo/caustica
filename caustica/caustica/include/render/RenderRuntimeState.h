#pragma once

#include <math/math.h>

#include <cstdint>
#include <string>

namespace caustica::render
{

struct RenderInvalidationState
{
    bool ShaderReloadRequested = false;
    bool AccelerationStructRebuildRequested = false;
    float ShaderAndACRefreshDelayedRequest = 0.0f;
};

struct GaussianSplatSceneSummary
{
    uint32_t SplatCount = 0;
    uint32_t ObjectCount = 0;
    std::string FileName;
};

struct RenderPickState
{
    dm::uint2 Position = { 0, 0 };
    bool MaterialRequested = false;
    bool InstanceRequested = false;

    void requestMaterialPick() { MaterialRequested = true; }
    bool hasActivePickRequest() const { return MaterialRequested || InstanceRequested; }
    void clearPickRequests()
    {
        MaterialRequested = false;
        InstanceRequested = false;
    }
};

struct RenderRuntimeState
{
    RenderInvalidationState Invalidation;
    GaussianSplatSceneSummary GaussianSplats;
    RenderPickState Picking;
};

} // namespace caustica::render
