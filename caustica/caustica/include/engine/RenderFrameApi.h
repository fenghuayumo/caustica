#pragma once

// ENGINE-INTERNAL schedule helpers — not part of the public API (see docs/public-api.md).
// Hosts drive frames via EngineApp::run / stepFrame only.

#include <cstdint>

struct PathTracerSettings;

namespace caustica
{

class App;
class GpuDevice;
struct SceneViewState;
struct Time;

// Schedule-facing frame helpers (also registered by scene plugins).
void beginFrameScheduled(App& app);
void animate(App& app, float elapsedTimeSeconds);
void tickSimulationAndFrameTiming(App& app, float elapsedTimeSeconds);
void tickSimulationAndFrameTiming(
    SceneViewState& viewState,
    const PathTracerSettings& settings,
    const Time& time);
void renderScene(App& app, GpuDevice& gpuDevice);
void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice);
void backBufferResizing(App& app);

void setSceneTime(App& app, double sceneTime);
[[nodiscard]] double sceneTime(const App& app);
[[nodiscard]] double& sceneTimeRef(App& app);

} // namespace caustica
