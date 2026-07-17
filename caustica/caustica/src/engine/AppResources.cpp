#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <engine/PathTracingRuntime.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <backend/GpuDevice.h>
#include <scene/SceneManager.h>
#include <render/worldRenderer/WorldRenderer.h>

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

SessionCamera* sessionCameraResource(const App& app)
{
    return const_cast<SessionCamera*>(app.tryResource<SessionCamera>());
}

SceneSession* sceneSession(const App& app)
{
    return const_cast<SceneSession*>(app.tryResource<SceneSession>());
}

PathTracingRuntime* pathTracingRuntime(const App& app)
{
    return const_cast<PathTracingRuntime*>(app.tryResource<PathTracingRuntime>());
}

::SceneManager* sceneManager(const App& app)
{
    if (SceneSession* session = sceneSession(app))
        return session->manager.get();
    return nullptr;
}

render::WorldRenderer* worldRenderer(const App& app)
{
    if (PathTracingRuntime* pt = pathTracingRuntime(app))
        return pt->worldRenderer();
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
