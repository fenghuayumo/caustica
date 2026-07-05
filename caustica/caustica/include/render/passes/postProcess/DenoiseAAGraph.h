#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/core/PathTracerSettings.h>
#include <render/FramePassRegistry.h>

class RenderTargets;
class AccumulationPass;

namespace caustica
{
class CameraController;
class ICompositeView;
}

namespace caustica::render
{
struct PathTracingFrameContext;
class TemporalAntiAliasingPass;
class WorldRenderer;
}

namespace caustica::rg
{

struct DenoiseAAGraphParams
{
    GraphBuilder& graph;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    render::FramePassRegistry* framePassRegistry = nullptr;
    render::PathTracingFrameContext* frameContext = nullptr;

    render::WorldRenderer* worldRenderer = nullptr;
    bool aaReset = false;

    caustica::CameraController* camera = nullptr;
    const caustica::ICompositeView* compositeView = nullptr;
    render::TemporalAntiAliasingPass* temporalAAPass = nullptr;
    AccumulationPass* accumulationPass = nullptr;

    uint64_t frameIndex = 0;
    int accumulationSampleIndex = 0;
    int* gaussianSplatTemporalSampleIndex = nullptr;
    bool* gaussianSplatTemporalReset = nullptr;
};

// Registers denoise/AA graph passes: copy, TAA, DLSS, accumulation, and registry hooks.
void buildDenoiseAndAAGraph(const DenoiseAAGraphParams& params);

} // namespace caustica::rg
