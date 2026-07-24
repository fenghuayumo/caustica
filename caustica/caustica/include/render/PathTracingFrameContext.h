#pragma once

#include <math/math.h>
#include <rhi/rhi.h>
#include <shaders/SampleConstantBuffer.h>

#include <functional>
#include <string>

struct PathTracerCameraData;
class RenderTargets;

namespace caustica::render
{

class WorldRenderer;

// Per-frame mutable state threaded through the path-tracing pass pipeline.
struct PathTracingFrameContext
{
    WorldRenderer* renderer = nullptr;
    caustica::rhi::IFramebuffer*      framebuffer = nullptr;

    dm::uint2 displaySize{};
    dm::uint2 renderSize{};
    float     displayAspectRatio = 1.f;
    float     lodBias = 0.f;

    bool needNewPasses = false;
    bool needNewBindings = false;
    bool exposureResetRequired = false;
    bool forcePathTracingShaderReload = false;

    PathTracerCameraData cameraData{};
    SampleConstants      constants{};

    // Set to true when a GPU sync point fails and the frame must abort.
    bool aborted = false;

    // Flushes the command list during renderer initialization stages.
    std::function<bool(const char* stage)> submitInitializationStage;

    caustica::rhi::ICommandList* commandList = nullptr;
    RenderTargets* renderTargets = nullptr;
    caustica::rhi::ITexture* ldrColor = nullptr;

    caustica::rhi::BindingSetDesc* bindingSetDesc = nullptr;
};

} // namespace caustica::render
