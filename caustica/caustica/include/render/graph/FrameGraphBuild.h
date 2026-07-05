#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/core/PathTracerSettings.h>
#include <math/math.h>
#include <rhi/nvrhi.h>
#include <shaders/SampleConstantBuffer.h>

class RenderTargets;
class AccumulationPass;
class PTPipelineVariant;
class ToneMappingPass;

namespace caustica
{
class CameraController;
class ICompositeView;
class BindingCache;
}

namespace caustica::render
{
class BloomPass;
class FramePassRegistry;
class FullscreenBlitPass;
class PathTracingFrameContext;
class TemporalAntiAliasingPass;
class WorldRenderer;
}

namespace caustica::rg
{

struct DenoiseAAGraphParams;
struct PostProcessGraphParams;

struct FrameGraphBuildParams
{
    GraphBuilder& graph;

    render::WorldRenderer* worldRenderer = nullptr;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    render::PathTracingFrameContext* frameContext = nullptr;
    render::FramePassRegistry* framePassRegistry = nullptr;
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    const SampleConstants* sampleConstants = nullptr;
    bool hasScene = true;

    // Denoise / AA
    caustica::CameraController* camera = nullptr;
    const caustica::ICompositeView* compositeView = nullptr;
    render::TemporalAntiAliasingPass* temporalAAPass = nullptr;
    AccumulationPass* accumulationPass = nullptr;
    uint64_t frameIndex = 0;
    int accumulationSampleIndex = 0;
    bool aaReset = false;
    int* gaussianSplatTemporalSampleIndex = nullptr;
    bool* gaussianSplatTemporalReset = nullptr;

    // Post-process
    render::BloomPass* bloomPass = nullptr;
    ToneMappingPass* toneMappingPass = nullptr;
    dm::uint2 displaySize{};
    nvrhi::BindingSetHandle pathTracingBindingSet;
    nvrhi::IDescriptorTable* descriptorTable = nullptr;
    PTPipelineVariant* testRaygenPpHdrPipeline = nullptr;
    PTPipelineVariant* edgeDetectionPipeline = nullptr;
    bool* outCommandListWasClosed = nullptr;

    // Composite
    caustica::BindingCache* bindingCache = nullptr;
    render::FullscreenBlitPass* blitPass = nullptr;
};

// Builds the full frame graph: PathTrace → NRD → Denoise/AA → PostProcess → Composite.
void buildFrameGraph(const FrameGraphBuildParams& params);

} // namespace caustica::rg
