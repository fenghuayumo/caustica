#pragma once

#include <functional>
#include <span>

struct DebugFeedbackStruct;

namespace nvrhi
{
class ICommandList;
class IFramebuffer;
class ITexture;
struct BindingSetDesc;
} // namespace nvrhi

class RenderTargets;

namespace caustica::render
{

// Fixed pipeline phases where optional host-side extensions may run.
// New editor features should hook an existing phase or add a new phase here —
// the engine never references editor types (ZoomTool, capture scripts, etc.).
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

struct PathTracingFrameEvent
{
    PathTracingFramePhase framePhase = PathTracingFramePhase::PreRender;

    nvrhi::ICommandList* commandList = nullptr;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    RenderTargets* renderTargets = nullptr;
    nvrhi::ITexture* ldrColor = nullptr;

    struct PathTraceDebug
    {
        bool pickActive = false;
        bool exploreDeltaTree = false;
    }* pathTraceDebug = nullptr;

    nvrhi::BindingSetDesc* bindingSetDesc = nullptr;

    const DebugFeedbackStruct* pickFeedback = nullptr;

    struct PostRenderData
    {
        nvrhi::ITexture* framebufferTexture = nullptr;
        std::function<bool(const char* fileName)>* saveFramebuffer = nullptr;
        bool experimentalScreenshotRequested = false;
    }* postRender = nullptr;
};

// Optional per-frame extension; default implementation is a no-op.
// Desktop editor registers SceneEditorFrameExtension; headless sessions pass an empty span.
class IPathTracingFrameExtension
{
public:
    virtual ~IPathTracingFrameExtension() = default;
    virtual void onFrameEvent(PathTracingFrameEvent& event) {}
};

} // namespace caustica::render
