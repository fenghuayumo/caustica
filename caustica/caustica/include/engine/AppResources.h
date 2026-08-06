#pragma once

#include <core/command_line.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>

namespace caustica
{

class App;
class GpuDevice;
class SceneViewState;
class CameraController;
struct GpuSharedCaches;
struct SceneSession;

// Prefer RenderSessionApi; WorldRenderer is engine-internal via internal/WorldRendererAccess.h.

[[nodiscard]] GpuDevice* gpuDevice(const App& app);
[[nodiscard]] GpuSharedCaches* gpuSharedCaches(const App& app);
[[nodiscard]] CameraController* cameraController(const App& app);
[[nodiscard]] SceneSession* sceneSession(const App& app);

[[nodiscard]] PathTracerSettings* settings(const App& app);
[[nodiscard]] render::RenderRuntimeState* runtimeState(const App& app);
[[nodiscard]] render::AppDiagnostics* diagnostics(const App& app);
[[nodiscard]] const CommandLineOptions* cmdLine(const App& app);
[[nodiscard]] SceneViewState* viewState(const App& app);

} // namespace caustica
