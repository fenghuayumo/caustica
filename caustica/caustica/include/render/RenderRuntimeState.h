#pragma once

#include <math/math.h>

#include <cstdint>
#include <string>

namespace caustica::render
{

struct RenderInvalidationState
{
    // Full RTPSO rebuild (Ctrl+R / source hot-reload / scene load). Prefer RtPipelineCache binds.
    bool ShaderReloadRequested = false;
    bool AccelerationStructRebuildRequested = false;
    // Legacy timer: now only schedules AccelerationStructRebuildRequested (never ShaderReload).
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
    // Cursor position in display/window pixels (not path-trace renderSize).
    // WorldRenderer maps to render pixels after DLSS settles for the frame.
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
