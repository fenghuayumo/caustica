#pragma once

namespace caustica
{

class App;

void registerSceneLoadingPlugin(App& app);
void registerSceneAnimationPlugin(App& app);
void registerCameraPlugin(App& app);
void registerPathTracingPlugin(App& app);
void registerRenderExtractPlugin(App& app);
void registerWindowTitlePlugin(App& app);

// Extract-only: upload meshes/AS for Scene::requestGpuStructureSync() before publish.
// Applications must not call this -- spawn/despawn only mark dirty; Extract flushes.
void flushPendingStructureGpu(App& app);

} // namespace caustica