#pragma once

namespace caustica
{

class App;

namespace sceneSession
{

void registerSceneLoadingPlugin(App& app);
void registerSceneAnimationPlugin(App& app);
void registerCameraPlugin(App& app);
void registerPathTracingPlugin(App& app);
void registerRenderExtractPlugin(App& app);
void registerWindowTitlePlugin(App& app);

// Extract-only: upload meshes/AS for Scene::requestGpuStructureSync() before publish.
// Applications mutate ECS via spawn/despawn; do not call this from game code.
void flushPendingStructureGpu(App& app);

} // namespace sceneSession

} // namespace caustica
