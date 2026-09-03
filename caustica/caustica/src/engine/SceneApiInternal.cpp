#include <engine/internal/SceneApiInternal.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SceneSession.h>
#include <engine/SceneLifecycle.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <engine/LoadSession.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

    static const auto traceStart = std::chrono::steady_clock::now();
    const double elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - traceStart).count();
    char line[1100];
    snprintf(line, sizeof(line), "[+%8.3fs] %s", elapsedSeconds, buf);

    caustica::info("%s", line);
#ifdef _WIN32
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
#endif
    std::ofstream f("scene_switch.log", std::ios::app);
    if (f)
        f << line << '\n';
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

void retargetAssetPackForSceneRequest(App& app, const std::string& sceneName)
{
    if (sceneName.empty())
        return;

    std::filesystem::path scenePath(sceneName);
    if (!scenePath.is_absolute() && !std::filesystem::exists(scenePath))
        return;

    std::error_code ec;
    if (!scenePath.is_absolute())
        scenePath = std::filesystem::absolute(scenePath, ec);
    if (ec)
        return;

    const std::filesystem::path pack = findAssetPackContaining(scenePath);
    if (pack.empty())
        return;

    const std::filesystem::path current = getAssetPackRoot();
    std::error_code eqEc;
    if (!current.empty() && std::filesystem::equivalent(pack, current, eqEc) && !eqEc)
        return;

    sceneSwitchTrace("retarget asset pack '%s' -> '%s' (scene '%s')",
        current.generic_string().c_str(),
        pack.generic_string().c_str(),
        sceneName.c_str());
    setAssetPackRootOverride(pack);
    if (::SceneManager* manager = sessionManager(app))
        manager->discoverAvailableScenes(pack);
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

    retargetAssetPackForSceneRequest(app, sceneName);

    if (isSceneSwitchBusy(app))
    {
        sceneSwitchTrace("applySceneSwitch: ignored, switch busy (phase=%s)",
            loadSessionPhaseName(vs->loadSession.phase));
        return;
    }

    if (!manager->beginSceneSwitch(sceneName, getLocalPath(c_AssetsFolder), forceReload))
    {
        sceneSwitchTrace("applySceneSwitch: ignored, same scene '%s' (forceReload=%d)",
            sceneName.c_str(), forceReload ? 1 : 0);
        return;
    }

    // Relative names resolve against the current pack; pin the pack that actually
    // contains the resolved scene.json so later getLocalPath("Assets") matches.
    retargetAssetPackForSceneRequest(app, manager->getCurrentScenePath().string());

    cfg->ResetAccumulation = true;
    cfg->ResetRealtimeCaches = true;
    // Keep DLSS/DLSS-RR off across the switch so Streamline is not recreated
    // against torn-down AS/material buffers on the first post-load frame.
    // Remember the requested mode and restore it once the new scene is ready
    // (see tickLoadSession / restoreDeferredRealtimeAA).
    if (cfg->RealtimeAA >= 2)
    {
        vs->deferredRealtimeAA = cfg->RealtimeAA;
        cfg->RealtimeAA = 1;
    }

    manager->setAsyncLoadingEnabled(true);

    vs->progressLoading.stop();
    vs->progressLoading.start("Loading scene...");
    vs->progressLoading.Set(5);

    vs->loadSession.reset();
    vs->loadSession.deferredImportPending = true;

    // If a live scene exists, finish GPU teardown before starting the CPU import worker
    // (async Teardown → tickLoadSession begins the deferred import).
    if (manager->getScene())
    {
        sceneSwitchTrace("applySceneSwitch: Teardown then deferred import '%s'", sceneName.c_str());
        caustica::onSceneUnloading(app); // LoadSession::Teardown, non-blocking GPU release
        manager->clearScene();
        return;
    }

    vs->loadSession.phase = LoadSessionPhase::Importing;
    vs->loadSession.deferredImportPending = false;
    vs->sceneGpuSuspended.store(false, std::memory_order_release);

    sceneSwitchTrace("applySceneSwitch: begin async load '%s'", sceneName.c_str());
    manager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        manager->getCurrentScenePath());
    sceneSwitchTrace("applySceneSwitch: beginLoadingScene returned (async worker running=%d)",
        manager->isSceneLoading() ? 1 : 0);

    if (!manager->isSceneLoading() && manager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        manager->clearScene();
        clearActiveScene(app);
        vs->progressLoading.stop();
        vs->loadSession.reset();
        vs->sceneGpuSuspended.store(false, std::memory_order_release);
    }
}

} // namespace caustica::detail
