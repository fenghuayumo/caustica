#include <engine/SceneApiInternal.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <memory>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneManager.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace caustica::detail
{

void sceneSwitchTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    caustica::info("%s", buf);
#ifdef _WIN32
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
    std::ofstream f("scene_switch.log", std::ios::app);
    if (f)
        f << buf << '\n';
}

CameraController* sessionCamera(App& app)
{
    if (SessionCamera* session = sessionCameraResource(app))
        return &session->camera;
    return nullptr;
}

const CameraController* sessionCamera(const App& app)
{
    return sessionCamera(const_cast<App&>(app));
}

::SceneManager* sessionManager(App& app)
{
    if (SceneSession* session = sceneSession(app))
        return session->manager.get();
    return nullptr;
}

::SceneManager* sessionManager(const App& app)
{
    return sessionManager(const_cast<App&>(app));
}

void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload)
{
    ::SceneManager* manager = sessionManager(app);
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    if (!manager || !cfg || !vs)
    {
        sceneSwitchTrace("applySceneSwitch: missing manager/settings/viewState");
        return;
    }

    if (manager->isSceneLoading())
    {
        sceneSwitchTrace("applySceneSwitch: ignored, scene already loading");
        return;
    }
    if (vs->sceneGpuSuspended.load(std::memory_order_acquire))
    {
        sceneSwitchTrace("applySceneSwitch: ignored, sceneGpuSuspended");
        return;
    }

    if (!manager->beginSceneSwitch(sceneName, getLocalPath(c_AssetsFolder), forceReload))
    {
        sceneSwitchTrace("applySceneSwitch: ignored, same scene '%s' (forceReload=%d)",
            sceneName.c_str(), forceReload ? 1 : 0);
        return;
    }

    // Stop new frame submit before unload/load. Cleared in onSceneLoaded / failure.
    vs->sceneGpuSuspended.store(true, std::memory_order_release);

    cfg->ResetAccumulation = true;
    cfg->ResetRealtimeCaches = true;
    // Keep DLSS/DLSS-RR off across the switch so Streamline is not recreated
    // against torn-down AS/material buffers on the first post-load frame.
    if (cfg->RealtimeAA >= 2)
        cfg->RealtimeAA = 1;

    // CPU import on a worker so the logic/UI thread keeps pumping. GPU bind still
    // runs on the logic thread when the worker finishes (via updateLoading).
    manager->setAsyncLoadingEnabled(true);

    vs->progressLoading.stop();
    vs->progressLoading.start("Loading scene...");
    vs->progressLoading.Set(5);
    sceneSwitchTrace("applySceneSwitch: begin async load '%s'", sceneName.c_str());
    manager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        manager->getCurrentScenePath());
    sceneSwitchTrace("applySceneSwitch: beginLoadingScene returned (async worker running=%d)",
        manager->isSceneLoading() ? 1 : 0);

    // Async: scene stays null until the worker finishes and updateLoading promotes it.
    // Sync fallback (if async disabled elsewhere): detect immediate failure.
    if (!manager->isSceneLoading() && manager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        manager->clearScene();
        clearActiveScene(app);
        vs->progressLoading.stop();
        vs->sceneGpuSuspended.store(false, std::memory_order_release);
    }
}

} // namespace caustica::detail
