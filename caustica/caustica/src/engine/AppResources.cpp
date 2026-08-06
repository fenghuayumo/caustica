#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/GpuSharedCaches.h>
#include <render/core/CameraController.h>
#include <engine/SceneSession.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <backend/GpuDevice.h>
#include <render/WorldRenderer.h>

using namespace caustica::render;

namespace caustica
{

GpuDevice* gpuDevice(const App& app)
{
    return app.getGpuDevice();
}

GpuSharedCaches* gpuSharedCaches(const App& app)
{
    return const_cast<GpuSharedCaches*>(app.tryResource<GpuSharedCaches>());
}

CameraController* cameraController(const App& app)
{
    return const_cast<CameraController*>(app.tryResource<CameraController>());
}

SceneSession* sceneSession(const App& app)
{
    return const_cast<SceneSession*>(app.tryResource<SceneSession>());
}

render::WorldRenderer* worldRenderer(const App& app)
{
    return const_cast<render::WorldRenderer*>(app.tryResource<render::WorldRenderer>());
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
