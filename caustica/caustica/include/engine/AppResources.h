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
class GpuRenderSubsystem;
class SceneViewState;

namespace render
{
class WorldRenderer;
}

[[nodiscard]] GpuRenderSubsystem* gpuRender(const App& app);
[[nodiscard]] GpuDevice* gpuDevice(const App& app);
[[nodiscard]] ::SceneManager* sceneManager(const App& app);
[[nodiscard]] render::WorldRenderer* worldRenderer(const App& app);

[[nodiscard]] PathTracerSettings* settings(const App& app);
[[nodiscard]] render::RenderRuntimeState* runtimeState(const App& app);
[[nodiscard]] render::AppDiagnostics* diagnostics(const App& app);
[[nodiscard]] const CommandLineOptions* cmdLine(const App& app);
[[nodiscard]] SceneViewState* viewState(const App& app);

} // namespace caustica
