#pragma once

#include <engine/Plugin.h>

#include <cstdint>

namespace caustica
{

class App;

// Scene runtime plugins (schedules). Registered by App::buildPlugins via registerSceneSchedules.
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

// Extract-only: upload meshes/AS for Scene::requestGpuStructureSync() after publish.
// Requires the current frame already published via extractAndPublishRenderSnapshot.
// Applications must not call this -- spawn/despawn only mark dirty; Extract flushes.
void flushPendingStructureGpu(App& app);

} // namespace caustica
