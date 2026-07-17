#pragma once

#include <core/command_line.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>

class SceneManager;

namespace caustica
{

class App;
class GpuDevice;
class SceneViewState;
struct GpuSharedCaches;
struct SessionCamera;
struct SceneSession;
struct PathTracingRuntime;

namespace render
{
class WorldRenderer;
}

[[nodiscard]] GpuDevice* gpuDevice(const App& app);
[[nodiscard]] GpuSharedCaches* gpuSharedCaches(const App& app);
[[nodiscard]] SessionCamera* sessionCameraResource(const App& app);
[[nodiscard]] SceneSession* sceneSession(const App& app);
[[nodiscard]] PathTracingRuntime* pathTracingRuntime(const App& app);
[[nodiscard]] ::SceneManager* sceneManager(const App& app);
[[nodiscard]] render::WorldRenderer* worldRenderer(const App& app);

[[nodiscard]] PathTracerSettings* settings(const App& app);
[[nodiscard]] render::RenderRuntimeState* runtimeState(const App& app);
[[nodiscard]] render::AppDiagnostics* diagnostics(const App& app);
[[nodiscard]] const CommandLineOptions* cmdLine(const App& app);
[[nodiscard]] SceneViewState* viewState(const App& app);

} // namespace caustica
