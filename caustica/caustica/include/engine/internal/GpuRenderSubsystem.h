#pragma once

// Engine-internal GPU bind / scene-load steps. Hosts must not include this.
// Prefer SceneLifecycle / RenderSessionApi / EntityWorld.

#include <cstddef>
#include <render/core/PathTracerSettings.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>

namespace caustica
{

class GpuDevice;
class AssetSystem;
struct GpuSharedCaches;
struct SceneSession;

namespace render
{
class WorldRenderer;
}
namespace scene { class SceneRenderData; }

struct gpuRenderSubsystemInitParams
{
    GpuDevice& gpuDevice;
    AssetSystem& assetSystem;
    GpuSharedCaches& gpuSharedCaches;
    SceneSession& sceneSession;
    render::WorldRenderer& worldRenderer;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;
    render::AppDiagnostics& diagnostics;
};

// RT/logic step executor for LoadSession (owner: SceneLifecycle::tickLoadSession).
// Do not add new Open Scene phase flags here — extend LoadSession.
class GpuRenderSubsystem
{
public:
    GpuRenderSubsystem();
    ~GpuRenderSubsystem();

    GpuRenderSubsystem(const GpuRenderSubsystem&) = delete;
    GpuRenderSubsystem& operator=(const GpuRenderSubsystem&) = delete;

    void shutdown();
    bool initialize(const gpuRenderSubsystemInitParams& params);

    void onSceneUnloading();

    [[nodiscard]] size_t pendingTextureFinalizeCount();
    // timeLimitMs > 0: budgeted finalize; <= 0: drain + loadingFinished.
    void flushTextures(float timeLimitMs);
    void bindWorld(const scene::SceneRenderData& renderData);
    // Returns next mesh index.
    size_t uploadMeshes(const scene::SceneRenderData& renderData, size_t meshBegin, size_t maxMeshes);
    void finalizeBind(const scene::SceneRenderData& renderData);
    // Logic-thread post-bind (AS flags, splat, etc.).
    void finishLoadedScene(const scene::SceneRenderData& renderData);

private:
    GpuSharedCaches* m_gpuSharedCaches = nullptr;
    SceneSession* m_sceneSession = nullptr;
    render::WorldRenderer* m_worldRenderer = nullptr;
    GpuDevice* m_gpuDevice = nullptr;
    AssetSystem* m_assetSystem = nullptr;
    PathTracerSettings* m_settings = nullptr;
    render::RenderRuntimeState* m_runtimeState = nullptr;
    render::AppDiagnostics* m_diagnostics = nullptr;
    bool m_shutdown = false;
};

} // namespace caustica
