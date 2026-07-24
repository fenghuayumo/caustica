#pragma once

#include <render/SceneGpuResources.h>
#include <render/core/PathTracerSettings.h>
#include <render/ecs/RenderFrameContext.h>
#include <math/math.h>
#include <rhi/nvrhi.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/SampleConstantBuffer.h>

#include <cstdint>
#include <memory>
#include <vector>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

class AccumulationPass;
class EnvMapProcessor;
class LightSamplingCache;
class MaterialGpuCache;
class OpacityMicromapBuilder;
class PostProcess;
class PTPipelineVariant;
class RenderTargets;
class RtxdiPass;
class ToneMappingPass;
struct GaussianSplatEmissionProxy;

namespace caustica
{
class AccelStructManager;
class BindingCache;
class CameraController;
class ICompositeView;
class IView;
}

namespace caustica::rg
{
class GraphBuilder;
}

namespace caustica::render
{

class BloomPass;
class DenoisePass;
class DLSS;
class FullscreenBlitPass;
class GaussianSplatFramePass;
class PathTracePass;
class PathTracingContext;
class PathTracingFrameContext;
class SceneGaussianSplatPasses;
class TemporalAntiAliasingPass;
class WorldRenderer;

// Shared graph-pass parameter bag (UE RDG AllocParameters style).
// Fill once per frame in WorldRenderer::makeFrameGraphContext; execute lambdas
// should use these fields instead of digging through WorldRenderer.
struct FrameGraphContext
{
    rg::GraphBuilder* graph = nullptr;
    WorldRenderer* renderer = nullptr; // leftover only; avoid in new execute bodies
    PathTracingFrameContext* frame = nullptr;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    SampleConstants* sampleConstants = nullptr;
    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies = nullptr;
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    const ExtractedFrameView* extractedView = nullptr;

    caustica::BindingCache* bindingCache = nullptr;
    FullscreenBlitPass* blitPass = nullptr;

    RtxdiPass* rtxdi = nullptr;
    PathTracePass* pathTrace = nullptr;
    DenoisePass* denoise = nullptr;
    GaussianSplatFramePass* gaussian = nullptr;
    // Non-owning: graph lambdas capture FrameGraphContext by value; owning
    // shared_ptrs here would keep EnvMapProcessor (etc.) alive until GraphBuilder
    // teardown and can unload textures after TextureLoader is already gone.
    EnvMapProcessor* environment = nullptr;

    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::IDescriptorTable* descriptorTable = nullptr;
    nvrhi::BufferHandle constantBuffer;

    PTPipelineVariant* ptBuildStablePlanes = nullptr;
    PTPipelineVariant* ptFillStablePlanes = nullptr;
    PTPipelineVariant* ptReference = nullptr;
    PTPipelineVariant* ptTestRaygenPPHDR = nullptr;
    PTPipelineVariant* ptEdgeDetection = nullptr;
    nvrhi::ComputePipelineHandle exportVBufferPSO;

    ToneMappingPass* toneMapping = nullptr;
    BloomPass* bloom = nullptr;
    TemporalAntiAliasingPass* temporalAntiAliasing = nullptr;
    AccumulationPass* accumulation = nullptr;
    PostProcess* postProcess = nullptr;

    // PathTraceLightingEnd → updateLightingEnd
    LightSamplingCache* lightSampling = nullptr;
    MaterialGpuCache* materials = nullptr;
    OpacityMicromapBuilder* opacityMaps = nullptr;
    SceneGpuFrameHandles gpuHandles{};
    nvrhi::BufferHandle subInstanceDataBuffer;

    PathTracingContext* pathTracingContext = nullptr;
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    caustica::AccelStructManager* accelStructs = nullptr;
    SceneGaussianSplatPasses* gaussianScenePasses = nullptr;
    caustica::CameraController* camera = nullptr;

    dm::uint2 renderSize{};
    dm::uint2 displaySize{};
    float displayAspectRatio = 1.f;
    dm::float2 cameraJitter{};
    uint32_t sampleIndex = 0;
    uint64_t frameIndex = 0;
    int accumulationSampleIndex = 0;
    bool accumulationCompleted = false;
    const caustica::IView* view = nullptr;
    const caustica::ICompositeView* compositeView = nullptr;

    bool hasScene = true;
    bool aaReset = false;
    bool* commandListWasClosed = nullptr;
    int* gaussianSplatTemporalSampleIndex = nullptr;
    bool* gaussianSplatTemporalReset = nullptr; // per-frame published reset
    bool* gaussianSplatOwnedTemporalReset = nullptr; // WR-owned sticky reset

#if CAUSTICA_WITH_STREAMLINE
    StreamlineInterface::DLSSRROptions* dlssRROptions = nullptr;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    DLSS* nativeDLSS = nullptr;
#endif

    // Debug overlay graph registration (filled by makeFrameGraphContext)
    bool showDebugLines = false;
    bool copyDebugFeedback = false;
    uint32_t capturedLineVertexCount = 0;
    std::vector<DebugLineStruct>* cpuSideDebugLines = nullptr;
    nvrhi::BufferHandle debugLineBufferCapture;
    nvrhi::BufferHandle debugLineBufferDisplay;
    nvrhi::BufferHandle feedbackBufferCpu;
    nvrhi::BufferHandle feedbackBufferGpu;
    nvrhi::BufferHandle debugDeltaPathTreeCpu;
    nvrhi::BufferHandle debugDeltaPathTreeGpu;
    nvrhi::BindingSetHandle linesBindingSet;
    nvrhi::GraphicsPipelineHandle linesPipeline;
};

} // namespace caustica::render
