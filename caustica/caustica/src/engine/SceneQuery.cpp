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
#include <render/WorldRenderer.h>

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
    // Path-tracer pick stores PTMaterial::gpuDataIndex. After the snapshot refactor,
    // PT materials live in MaterialGpuCache (not MaterialEx::ptData on scene materials).
    if (materialID < 0)
        return nullptr;

    auto* wr = worldRenderer(app);
    const auto& cache = wr ? wr->lightingPasses().materials() : nullptr;
    const std::shared_ptr<PTMaterial> ptFromCache =
        cache ? cache->findByGpuDataIndex(uint(materialID)) : nullptr;

    if (ptFromCache)
    {
        // Prefer a live scene MaterialEx so the editor keeps a stable identity;
        // re-link ptData for PTMaterial::safeCast / Material Editor.
        if (const std::shared_ptr<Scene> active = activeScene(app))
        {
            for (const auto& mat : active->getMaterials())
            {
                auto materialEx = std::dynamic_pointer_cast<MaterialEx>(mat);
                if (!materialEx || !mat)
                    continue;
                if (cache->findByResourceId(mat->renderResourceId).get() != ptFromCache.get())
                    continue;
                materialEx->ptData = ptFromCache;
                return mat;
            }
        }

        // No scene counterpart (or id mismatch) — wrap for Material Editor only.
        auto wrap = std::make_shared<MaterialEx>();
        wrap->ptData = ptFromCache;
        wrap->name = ptFromCache->name;
        wrap->modelFileName = ptFromCache->modelName;
        return wrap;
    }

    // Fallback: dense scene-list Material::materialID (can diverge from gpuDataIndex).
    const std::shared_ptr<Scene> active = activeScene(app);
    if (!active)
        return nullptr;

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
