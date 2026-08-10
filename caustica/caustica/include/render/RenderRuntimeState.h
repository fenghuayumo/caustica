#pragma once

#include <math/math.h>

#include <cstdint>
#include <string>

namespace caustica::render
{

struct RenderInvalidationState
{
    // One-shot request consumed by the renderer and applied to its frame-local settings snapshot.
    bool AccumulationResetRequested = false;
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
    // Monotonic request identities keep feedback from an older in-flight frame
    // from completing or clearing a newer click of the same type.
    uint64_t MaterialRequestId = 0;
    uint64_t InstanceRequestId = 0;

    void requestMaterialPick()
    {
        ++MaterialRequestId;
        MaterialRequested = true;
    }
    void requestInstancePick()
    {
        ++InstanceRequestId;
        InstanceRequested = true;
    }
    [[nodiscard]] bool isCurrentMaterialRequest(const RenderPickState& completed) const
    {
        return MaterialRequested
            && completed.MaterialRequested
            && MaterialRequestId == completed.MaterialRequestId;
    }
    [[nodiscard]] bool isCurrentInstanceRequest(const RenderPickState& completed) const
    {
        return InstanceRequested
            && completed.InstanceRequested
            && InstanceRequestId == completed.InstanceRequestId;
    }
    void completeMaterialPick(uint64_t requestId)
    {
        if (MaterialRequested && MaterialRequestId == requestId)
            MaterialRequested = false;
    }
    void completeInstancePick(uint64_t requestId)
    {
        if (InstanceRequested && InstanceRequestId == requestId)
            InstanceRequested = false;
    }
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
