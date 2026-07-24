#pragma once

#include <engine/Plugin.h>

#include <cstdint>

namespace caustica
{

class App;

// Scene runtime plugins (schedules). DefaultPlugins adds these; App::buildPlugins
// still calls registerSceneSchedules as a fallback when they were not registered.
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
    void configureSchedules(App& app) override;
};

struct WindowTitlePlugin : Plugin
{
    void configureSchedules(App& app) override;
};

// Schedule entry points implemented by the plugins above / RenderFrameApi.
void updateCamera(App& app, float elapsedTimeSeconds);
void updateWindowTitle(App& app);
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);

// Extract-only: enqueue async mesh/AS build for Scene::requestGpuStructureSync() after publish.
// Requires the current frame already published via extractAndPublishRenderSnapshot.
// Applications must not call this -- spawn/despawn only mark dirty; Extract enqueues.
// Returns false when a prior structure build is still in flight (pending flag kept).
bool enqueuePendingStructureGpu(App& app);
// Fallback when there is no committed proxy packet to serve during async build.
void flushPendingStructureGpuSync(App& app);

} // namespace caustica
