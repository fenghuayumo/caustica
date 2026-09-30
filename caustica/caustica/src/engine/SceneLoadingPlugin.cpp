#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/AppSchedules.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>

#include <assets/AssetSystem.h>
#include <core/log.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace caustica
{
namespace
{

bool processPendingSceneSwitch(App& app)
{
    SceneViewState* vs = caustica::viewState(app);
    if (!vs)
        return false;

    std::optional<SceneViewState::PendingSceneSwitch> pending;
    {
        std::lock_guard lock(vs->pendingSceneSwitchMutex);
        pending.swap(vs->pendingSceneSwitch);
    }

    if (!pending)
        return false;

    detail::applySceneSwitch(app, pending->sceneName, pending->forceReload);
    return true;
}

bool processHotReloadChanges(App& app)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    ::SceneManager* manager = detail::sessionManager(app);
    if (!assets || !manager || isSceneLoading(app))
        return false;

    const std::vector<HotReloadChange> changes = assets->pollHotReloadChanges();
    if (changes.empty())
        return false;

    const std::string sceneName = manager->getCurrentSceneName();
    const std::filesystem::path scenePath = manager->getCurrentScenePath();
    if (sceneName.empty() || isInlineScenePath(scenePath))
        return false;

    caustica::info("Hot reload: detected %zu asset source change(s), reloading scene '%s'",
        changes.size(), sceneName.c_str());
    detail::applySceneSwitch(app, sceneName, true);
    return true;
}

void tickSceneSwitchTest(App& app)
{
    const CommandLineOptions* cmd = caustica::cmdLine(app);
    SceneViewState* vs = caustica::viewState(app);
    if (!cmd || !vs || cmd->sceneSwitchTestInterval <= 0)
        return;

    ::SceneManager* manager = detail::sessionManager(app);
    if (!manager)
        return;

    if (--vs->sceneSwitchTestFramesUntilSwitch > 0)
        return;

    vs->sceneSwitchTestFramesUntilSwitch = cmd->sceneSwitchTestInterval;

    const std::vector<std::string>& scenes = caustica::availableScenes(app);
    if (scenes.size() < 2)
        return;

    if (vs->sceneSwitchTestSceneIndex >= scenes.size())
        vs->sceneSwitchTestSceneIndex = 0;

    const std::string& nextScene = scenes[vs->sceneSwitchTestSceneIndex++];
    caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
    caustica::setCurrentScene(app, nextScene);

    ++vs->sceneSwitchTestSwitchesDone;
    if (cmd->sceneSwitchTestCount > 0
        && vs->sceneSwitchTestSwitchesDone >= cmd->sceneSwitchTestCount)
    {
        app.requestExit();
    }
}

void beginFrame(App& app)
{
    if (!processPendingSceneSwitch(app))
        processHotReloadChanges(app);
    tickSceneSwitchTest(app);
}

} // namespace

void SceneLoadingPlugin::configureSchedules(App& app)
{
    app.addSystemAfter<system_label::SceneBeginFrame, system_label::SyncRenderThread>(
        AppSchedule::First,
        [](SystemContext& ctx) {
            beginFrame(ctx.app);
        });
}

} // namespace caustica
