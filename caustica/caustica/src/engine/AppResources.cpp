#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <backend/GpuDevice.h>
#include <scene/SceneManager.h>
#include <render/worldRenderer/WorldRenderer.h>

using namespace caustica::render;

namespace caustica
{

GpuRenderSubsystem* gpuRender(const App& app)
{
    return const_cast<GpuRenderSubsystem*>(app.tryResource<GpuRenderSubsystem>());
}

GpuDevice* gpuDevice(const App& app)
{
    return app.getGpuDevice();
}

::SceneManager* sceneManager(const App& app)
{
    if (GpuRenderSubsystem* gr = gpuRender(app))
        return gr->sceneManager();
    return nullptr;
}

render::WorldRenderer* worldRenderer(const App& app)
{
    if (GpuRenderSubsystem* gr = gpuRender(app))
        return gr->worldRenderer();
    return nullptr;
}

PathTracerSettings* settings(const App& app)
{
    return const_cast<App&>(app).tryResource<PathTracerSettings>();
}

RenderRuntimeState* runtimeState(const App& app)
{
    return const_cast<App&>(app).tryResource<RenderRuntimeState>();
}

AppDiagnostics* diagnostics(const App& app)
{
    return const_cast<App&>(app).tryResource<AppDiagnostics>();
}

const CommandLineOptions* cmdLine(const App& app)
{
    return const_cast<App&>(app).tryResource<CommandLineOptions>();
}

SceneViewState* viewState(const App& app)
{
    return const_cast<App&>(app).tryResource<SceneViewState>();
}

} // namespace caustica
