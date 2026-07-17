#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneQuery.h>
#include <engine/SceneAccess.h>
#include <engine/SceneSession.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>

using namespace caustica::render;

namespace caustica
{
namespace
{

::SceneManager* sessionManager(const App& app)
{
    if (SceneSession* session = sceneSession(app))
        return session->manager.get();
    return nullptr;
}

} // namespace

std::shared_ptr<Scene> activeScene(const App& app)
{
    if (auto* access = const_cast<App&>(app).tryResource<SceneAccess>())
    {
        if (access->active)
            return access->active;
    }
    ::SceneManager* manager = sessionManager(app);
    return manager ? manager->getScene() : nullptr;
}

void syncSceneAccess(App& app)
{
    auto* access = app.tryResource<SceneAccess>();
    if (!access)
        return;
    if (::SceneManager* manager = sessionManager(app))
        access->active = manager->getScene();
    else
        access->active.reset();
}

scene::SceneEntityWorld* entityWorld(const App& app)
{
    if (auto* access = const_cast<App&>(app).tryResource<SceneAccess>())
    {
        if (scene::SceneEntityWorld* ew = access->entityWorld())
            return ew;
    }
    const std::shared_ptr<Scene> active = activeScene(app);
    return active ? active->getEntityWorld() : nullptr;
}

ecs::World* sceneEcs(const App& app)
{
    scene::SceneEntityWorld* ew = entityWorld(app);
    return ew ? &ew->world() : nullptr;
}

const std::vector<std::string>& availableScenes(const App& app)
{
    static const std::vector<std::string> kEmpty;
    ::SceneManager* manager = sessionManager(app);
    return manager ? manager->getAvailableScenes() : kEmpty;
}

std::string currentSceneName(const App& app)
{
    ::SceneManager* manager = sessionManager(app);
    return manager ? manager->getCurrentSceneName() : std::string();
}

std::filesystem::path currentScenePath(const App& app)
{
    ::SceneManager* manager = sessionManager(app);
    return manager ? manager->getCurrentScenePath() : std::filesystem::path{};
}

bool isSceneStructureBusy(const App& app)
{
    ::SceneManager* manager = sessionManager(app);
    return manager && manager->isSceneStructureBusy();
}

bool shouldSkipRender(const App& app)
{
    return activeScene(app) == nullptr;
}

bool isSceneLoading(const App& app)
{
    ::SceneManager* manager = sessionManager(app);
    return manager && manager->isSceneLoading();
}

bool isSceneLoaded(const App& app)
{
    ::SceneManager* manager = sessionManager(app);
    return manager && manager->isSceneLoaded();
}

bool shouldRenderWhenUnfocused(const App& app)
{
    auto* wr = worldRenderer(app);
    PathTracerSettings* cfg = settings(app);
    if (!wr || !cfg)
        return true;

    if (wr->getFrameIndex() < 16 || cfg->ResetAccumulation || cfg->ResetRealtimeCaches)
        return true;

    return (!cfg->RealtimeMode && (wr->getAccumulationSampleIndex() < cfg->AccumulationTarget));
}

std::shared_ptr<Material> findMaterial(const App& app, int materialID)
{
    // Path-tracer pick / Material Editor use PTMaterial::gpuDataIndex.
    // Material::materialID is a dense scene-list index and can diverge after imports.
    if (materialID < 0)
        return nullptr;

    const std::shared_ptr<Scene> active = activeScene(app);
    if (!active)
        return nullptr;

    for (const auto& mat : active->getMaterials())
    {
        const auto pt = PTMaterial::safeCast(mat);
        if (pt && int(pt->gpuDataIndex) == materialID)
            return mat;
    }
    for (const auto& mat : active->getMaterials())
    {
        if (mat && mat->materialID == materialID)
            return mat;
    }
    return nullptr;
}

ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex)
{
    return SceneManager::findEntityByInstanceIndex(activeScene(app), instanceIndex);
}

} // namespace caustica
