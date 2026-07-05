#pragma once

#include <render/core/PathTracerSettings.h>
#include <rhi/nvrhi.h>
#include <shaders/SampleConstantBuffer.h>

class RenderTargets;

namespace caustica
{
class BindingCache;
}

namespace caustica::rg
{
class GraphBuilder;
}

namespace caustica::render
{

class FullscreenBlitPass;
class PathTracingFrameContext;
class WorldRenderer;
struct ExtractedFrameView;

// Pointer-based so graph execute lambdas can capture a copy by value safely.
struct RenderModuleContext
{
    rg::GraphBuilder* graph = nullptr;
    WorldRenderer* renderer = nullptr;
    PathTracingFrameContext* frame = nullptr;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    const SampleConstants* sampleConstants = nullptr;
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    const ExtractedFrameView* extractedView = nullptr;

    caustica::BindingCache* bindingCache = nullptr;
    FullscreenBlitPass* blitPass = nullptr;

    bool hasScene = true;
    bool aaReset = false;
    bool* commandListWasClosed = nullptr;
    int* gaussianSplatTemporalSampleIndex = nullptr;
    bool* gaussianSplatTemporalReset = nullptr;
};

} // namespace caustica::render
