#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <shaders/SampleConstantBuffer.h>

#include <functional>
#include <string>

struct DebugFeedbackStruct;
struct PathTracerCameraData;
class RenderTargets;

namespace nvrhi
{
class ICommandList;
class IFramebuffer;
class ITexture;
struct BindingSetDesc;
} // namespace nvrhi

namespace caustica::render
{

class WorldRenderer;

// Phases where optional host-side frame passes may run.
enum class PathTracingFramePhase
{
    PreRender,
    RenderTargetsRecreated,
    IdleMaintenance,
    BeforePathTrace,
    PopulateBindingSet,
    BeforeFinalBlit,
    AfterPickResolved,
    PostRender,
};

// Per-frame mutable state threaded through the path-tracing pass pipeline.
struct PathTracingFrameContext
{
    WorldRenderer* renderer = nullptr;
    nvrhi::IFramebuffer*      framebuffer = nullptr;

    dm::uint2 displaySize{};
    dm::uint2 renderSize{};
    float     displayAspectRatio = 1.f;
    float     lodBias = 0.f;

    bool needNewPasses = false;
    bool needNewBindings = false;
    bool exposureResetRequired = false;

    PathTracerCameraData cameraData{};
    SampleConstants      constants{};

    // Set to true when a GPU sync point fails and the frame must abort.
    bool aborted = false;

    // Flushes the command list during renderer initialization stages.
    std::function<bool(const char* stage)> submitInitializationStage;

    // Active phase when executing optional host-side frame passes.
    PathTracingFramePhase framePhase = PathTracingFramePhase::PreRender;

    nvrhi::ICommandList* commandList = nullptr;
    RenderTargets* renderTargets = nullptr;
    nvrhi::ITexture* ldrColor = nullptr;

    struct PathTraceDebug
    {
        bool pickActive = false;
        bool exploreDeltaTree = false;
    } pathTraceDebug;

    nvrhi::BindingSetDesc* bindingSetDesc = nullptr;

    const DebugFeedbackStruct* pickFeedback = nullptr;

    struct PostRenderData
    {
        nvrhi::ITexture* framebufferTexture = nullptr;
        std::function<bool(const char* fileName)>* saveFramebuffer = nullptr;
        bool experimentalScreenshotRequested = false;
    } postRender;
};

} // namespace caustica::render
