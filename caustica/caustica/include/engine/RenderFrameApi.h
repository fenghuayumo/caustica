#pragma once

// ENGINE-INTERNAL schedule helpers — not part of the public API (see docs/public-api.md).
// Hosts drive frames via EngineApp::run / stepFrame only.

#include <cstdint>

namespace caustica
{

class App;
class GpuDevice;

// Schedule-facing frame helpers (also registered by scene plugins).
void beginFrameScheduled(App& app);
void animate(App& app, float elapsedTimeSeconds);
void tickSimulationAndFrameTiming(App& app, float elapsedTimeSeconds);
void renderScene(App& app, GpuDevice& gpuDevice);
void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice);
void backBufferResizing(App& app);

void setSceneTime(App& app, double sceneTime);
[[nodiscard]] double sceneTime(const App& app);
[[nodiscard]] double& sceneTimeRef(App& app);

} // namespace caustica
