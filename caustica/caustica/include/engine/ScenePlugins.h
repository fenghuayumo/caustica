#pragma once

#include <cstdint>

namespace caustica
{

class App;

void registerSceneLoadingPlugin(App& app);
void registerSceneAnimationPlugin(App& app);
void registerCameraPlugin(App& app);
void registerPathTracingPlugin(App& app);
void registerRenderExtractPlugin(App& app);
void registerWindowTitlePlugin(App& app);

// Schedule entry points implemented by the plugins above / RenderFrameApi.
void updateCamera(App& app, float elapsedTimeSeconds);
void updateWindowTitle(App& app);
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);


// Extract-only: upload meshes/AS for Scene::requestGpuStructureSync() before publish.
// Applications must not call this -- spawn/despawn only mark dirty; Extract flushes.
void flushPendingStructureGpu(App& app);

} // namespace caustica