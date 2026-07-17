#include <engine/SceneApiInternal.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <engine/SceneViewState.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <memory>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneManager.h>

namespace caustica::detail
{

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
        return;

    if (!manager->beginSceneSwitch(sceneName, getLocalPath(c_AssetsFolder), forceReload))
        return;

    cfg->ResetAccumulation = true;
    cfg->ResetRealtimeCaches = true;
    manager->setAsyncLoadingEnabled(false);

    vs->progressLoading.stop();
    vs->progressLoading.start("Loading scene...");
    manager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        manager->getCurrentScenePath());
    if (manager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        manager->clearScene();
        vs->progressLoading.stop();
    }
}

} // namespace caustica::detail
