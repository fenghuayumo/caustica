#pragma once

#include <render/core/PathTracerSettings.h>
#include <rhi/nvrhi.h>
#include <math/math.h>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

class RenderTargets;
class AccumulationPass;
class PostProcess;

namespace caustica
{
class CameraController;
class GpuDevice;

namespace render
{
class TemporalAntiAliasingPass;
}

struct PostProcessAAParams
{
    PathTracerSettings&                     settings;
    nvrhi::ICommandList*                    commandList = nullptr;
    RenderTargets*                          renderTargets = nullptr;
    GpuDevice*                              gpuDevice = nullptr;

    dm::uint2                               renderSize{};
    dm::uint2                               displaySize{};
    float                                   displayAspectRatio = 1.f;
    dm::float2                              cameraJitter{};

    uint32_t                                sampleIndex = 0;
    uint64_t                                frameIndex = 0;
    bool                                    reset = false;

    render::TemporalAntiAliasingPass*       temporalAAPass = nullptr;
    AccumulationPass*                       accumulationPass = nullptr;
    PostProcess*                            postProcess = nullptr;

    nvrhi::BindingSetHandle                 bindingSet;
    nvrhi::BindingLayoutHandle              bindingLayout;
    nvrhi::BufferHandle                     constantBuffer;

    int                                     accumulationSampleIndex = 0;
    int*                                    gaussianSplatTemporalSampleIndex = nullptr;
    bool*                                   gaussianSplatTemporalReset = nullptr;

#if CAUSTICA_WITH_STREAMLINE
    StreamlineInterface::DLSSRROptions*     dlssRROptions = nullptr;
#endif
};

void postProcessAA(CameraController& camera, PostProcessAAParams& params);

// Platform-only AA paths (Streamline DLSS / native DLSS). Copy, TAA, and accumulation
// are handled by the frame graph (buildDenoiseAndAAGraph).
void postProcessAAPlatform(CameraController& camera, PostProcessAAParams& params);

} // namespace caustica
