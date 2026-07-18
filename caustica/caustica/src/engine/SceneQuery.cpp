#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneQuery.h>
#include <engine/ActiveScene.h>
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
    if (const ActiveScene* active = tryActiveScene(app))
        return active->scene;
    return nullptr;
}

const ActiveScene* tryActiveScene(const App& app)
{
    return const_cast<App&>(app).tryResource<ActiveScene>();
}

void commitActiveScene(
    App& app,
    std::shared_ptr<Scene> scene,
    std::string name,
    std::filesystem::path path)
{
    ActiveScene* active = app.tryResource<ActiveScene>();
    if (!active)
        return;

    active->scene = std::move(scene);
    active->name = std::move(name);
    active->path = std::move(path);
    ++active->generation;
}

void commitActiveSceneFromManager(App& app)
{
    ::SceneManager* manager = sessionManager(app);
    if (!manager)
    {
        clearActiveScene(app);
        return;
    }

    commitActiveScene(
        app,
        manager->getScene(),
        manager->getCurrentSceneName(),
        manager->getCurrentScenePath());
}

void clearActiveScene(App& app)
{
    ActiveScene* active = app.tryResource<ActiveScene>();
    if (!active)
        return;

    active->scene.reset();
    active->name.clear();
    active->path.clear();
    ++active->generation;
}

scene::SceneEntityWorld* entityWorld(const App& app)
{
    if (const ActiveScene* active = tryActiveScene(app))
        return active->entityWorld();
    return nullptr;
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
    if (const ActiveScene* active = tryActiveScene(app))
        return active->name;
    return {};
}

std::filesystem::path currentScenePath(const App& app)
{
    if (const ActiveScene* active = tryActiveScene(app))
        return active->path;
    return {};
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
