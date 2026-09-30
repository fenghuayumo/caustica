#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/RenderFrameApi.h>
#include <engine/RenderFramebufferOverride.h>
#include <engine/GpuSharedCaches.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/RenderTextureDebugApi.h>
#include <engine/Time.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <assets/AssetSystem.h>
#include <scene/SceneManager.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <scene/scene_utils.h>
#include <engine/MeshDeformApi.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/WorldRenderer.h>
#include <render/RenderRuntimeState.h>
#include <backend/GpuDevice.h>
#include <core/format.h>

using namespace caustica::render;

namespace caustica
{
void updateFpsInfo(
    SceneViewState& viewState,
    const PathTracerSettings& settings,
    double frameTimeSeconds)
{
    if (frameTimeSeconds <= 0.0)
        return;

#if CAUSTICA_WITH_STREAMLINE
    if (settings.actualDLSSFGMode() != SI::DLSSGMode::eOff)
    {
        uint32_t presentedFrames = settings.DLSSFGMultiplier;
        if (presentedFrames == 0)
            presentedFrames = 1u + settings.DLSSFGNumFramesToGenerate;

        // Fixed-width fields avoid ImGui layout flicker in narrow docks.
        viewState.fpsInfo = stringFormat("%6.2f ms/%u-frames* (%5.1f FPS*) *DLSS-G",
            frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
        return;
    }
#endif

    viewState.fpsInfo = stringFormat(
        "%6.2f ms/frame (%5.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
}

void updateFpsInfo(App& app, double frameTimeSeconds)
{
    SceneViewState* viewState = caustica::viewState(app);
    PathTracerSettings* settings = caustica::settings(app);
    if (viewState && settings)
        updateFpsInfo(*viewState, *settings, frameTimeSeconds);
}

} // namespace caustica

namespace
{
    void recordFrameTiming(App& app, const GpuDevice& gpuDevice)
    {
        SceneViewState* vs = caustica::viewState(app);
        double frameTime = gpuDevice.getAverageFrameTimeSeconds();
        if (frameTime <= 0.0 && vs && vs->lastDeltaTime > 0.0f)
            frameTime = static_cast<double>(vs->lastDeltaTime);
        caustica::updateFpsInfo(app, frameTime);
    }

    void afterWorldRenderDefault(App& app, GpuDevice& /*gpuDevice*/)
    {
        RenderRuntimeState* runtime = caustica::runtimeState(app);
        if (!runtime)
            return;

        const auto* wr = caustica::worldRenderer(app);
        const caustica::render::RenderPickState renderedPick = wr
            ? wr->getLastRenderedPicking()
            : caustica::render::RenderPickState{};
        if (renderedPick.MaterialRequested)
            runtime->Picking.completeMaterialPick(renderedPick.MaterialRequestId);
        if (renderedPick.InstanceRequested)
            runtime->Picking.completeInstancePick(renderedPick.InstanceRequestId);
    }

    void afterWorldRender(App& app, GpuDevice& gpuDevice)
    {
        afterWorldRenderDefault(app, gpuDevice);
    }
}

using namespace caustica::render;

namespace caustica
{

void renderScene(App& app, GpuDevice& gpuDevice)
{
    if (shouldSkipRender(app))
        return;

    auto* wr = worldRenderer(app);
    if (!wr)
        return;

    caustica::rhi::Framebuffer* target = gpuDevice.getCurrentFramebuffer(true);
    if (auto* overrideFb = app.tryResource<RenderFramebufferOverride>();
        overrideFb && overrideFb->framebuffer)
    {
        target = overrideFb->framebuffer;
    }

    wr->render(target);
    recordFrameTiming(app, gpuDevice);
}

void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice)
{
    ::afterWorldRender(app, gpuDevice);
}

void backBufferResizing(App& app)
{
    if (auto* wr = worldRenderer(app))
        wr->onBackBufferResizing();
}

void setSceneTime(App& app, double sceneTime)
{
    assert(viewState(app));
    viewState(app)->sceneTime = sceneTime;
}

double sceneTime(const App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

double& sceneTimeRef(App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

} // namespace caustica

uint32_t caustica::debugViewTextureCount(const App& app)
{
    const render::WorldRenderer* wr = worldRenderer(app);
    return wr ? wr->debugViewTextureCount() : 0;
}

bool caustica::debugViewTextureInfo(
    const App& app,
    uint32_t index,
    std::string* outName,
    rhi::Texture** outTexture)
{
    const render::WorldRenderer* wr = worldRenderer(app);
    return wr ? wr->debugViewTextureInfo(index, outName, outTexture) : false;
}

caustica::rhi::Texture* caustica::findDebugViewTexture(const App& app, std::string_view name)
{
    const render::WorldRenderer* wr = worldRenderer(app);
    return wr ? wr->findDebugViewTexture(name) : nullptr;
}

void caustica::requestDebugViewTextureCapture(App& app, std::string_view name)
{
    if (render::WorldRenderer* wr = worldRenderer(app))
        wr->requestDebugViewTextureCapture(name);
}

void caustica::clearDebugViewTextureCapture(App& app)
{
    if (render::WorldRenderer* wr = worldRenderer(app))
        wr->clearDebugViewTextureCapture();
}
