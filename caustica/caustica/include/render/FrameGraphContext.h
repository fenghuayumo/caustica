#pragma once

#include <render/SceneGpuResources.h>
#include <render/core/PathTracerSettings.h>
#include <render/ecs/RenderFrameContext.h>
#include <rhi/nvrhi.h>
#include <shaders/SampleConstantBuffer.h>

#include <cstdint>
#include <memory>

class AccumulationPass;
class LightSamplingCache;
class MaterialGpuCache;
class OpacityMicromapBuilder;
class PostProcess;
class PTPipelineVariant;
class RenderTargets;
class RtxdiPass;
class ToneMappingPass;

namespace caustica
{
class BindingCache;
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
class FullscreenBlitPass;
class PathTracePass;
class PathTracingFrameContext;
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
    const SampleConstants* sampleConstants = nullptr;
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    const ExtractedFrameView* extractedView = nullptr;

    caustica::BindingCache* bindingCache = nullptr;
    FullscreenBlitPass* blitPass = nullptr;

    RtxdiPass* rtxdi = nullptr;
    PathTracePass* pathTrace = nullptr;

    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::IDescriptorTable* descriptorTable = nullptr;

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
    std::shared_ptr<MaterialGpuCache> materials;
    std::shared_ptr<OpacityMicromapBuilder> opacityMaps;
    SceneGpuFrameHandles gpuHandles{};
    nvrhi::BufferHandle subInstanceDataBuffer;

    uint64_t frameIndex = 0;
    int accumulationSampleIndex = 0;
    const caustica::IView* view = nullptr;
    const caustica::ICompositeView* compositeView = nullptr;

    bool hasScene = true;
    bool aaReset = false;
    bool* commandListWasClosed = nullptr;
    int* gaussianSplatTemporalSampleIndex = nullptr;
    bool* gaussianSplatTemporalReset = nullptr;
};

} // namespace caustica::render
