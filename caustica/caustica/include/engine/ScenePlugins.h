#pragma once

// ENGINE-INTERNAL — not part of the public API (see docs/public-api.md).

#include <engine/Plugin.h>

#include <cstdint>
#include <memory>

namespace caustica
{

class App;
class Scene;
enum class StructureGpuUploadMode : uint8_t;
namespace scene { class SceneRenderData; }

// Scene runtime plugins (schedules). DefaultPlugins adds these.
struct SceneLoadingPlugin : Plugin
{
    void configureSchedules(App& app) override;
};

struct SceneAnimationPlugin : Plugin
{
    void configureSchedules(App& app) override;
};

struct CameraPlugin : Plugin
{
    void configureSchedules(App& app) override;
};

struct PathTracingPlugin : Plugin
{
    void configureSchedules(App& app) override;
};

struct RenderExtractPlugin : Plugin
{
    void build(App& app) override;
    void configureSchedules(App& app) override;
};

struct WindowTitlePlugin : Plugin
{
    void configureSchedules(App& app) override;
};

// Schedule entry points implemented by the plugins above / RenderFrameApi.
void updateCamera(App& app, float elapsedTimeSeconds);
void resolveActiveCamera(App& app);
void updateWindowTitle(App& app);
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);

// Extract-only: enqueue async mesh/AS build for Scene::requestGpuStructureSync() after publish.
// Requires the current frame already published via extractAndPublishRenderSnapshot.
// Applications must not call this -- spawn/despawn only mark dirty; Extract enqueues.
// Returns false when a prior structure build is still in flight (pending flag kept).
bool enqueuePendingStructureGpu(App& app);

// Render-domain implementation shared by normal structure edits and the
// initial scene-load transaction. The load path requests fence completion
// before it publishes the scene to normal rendering.
[[nodiscard]] bool buildSceneGpuStructure(
    App& app,
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<const scene::SceneRenderData>& renderData,
    StructureGpuUploadMode uploadMode,
    uint32_t frameIndex,
    bool waitForCompletion);

} // namespace caustica
