#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneQuery.h>
#include <engine/ActiveScene.h>
#include <engine/SceneSession.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/internal/RenderResourceAccess.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/core/PathTracerSettings.h>
#include <render/WorldRenderer.h>

using namespace caustica::render;
using caustica::scene::internal::RenderResourceAccess;

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
        return active->m_scene;
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

    scene::SceneEntityWorld* oldWorld = entityWorld(app);
    scene::SceneEntityWorld* incoming = scene ? scene->getEntityWorld() : nullptr;
    if (oldWorld && oldWorld != incoming)
        oldWorld->resetScene();

    if (scene)
        scene->adoptLiveEcs(app.world());

    active->m_scene = std::move(scene);
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

    active->m_scene.reset();
    active->name.clear();
    active->path.clear();
    ++active->generation;
}

scene::SceneEntityWorld* entityWorld(const App& app)
{
    if (const ActiveScene* active = tryActiveScene(app))
        return active->m_scene ? active->m_scene->getEntityWorld() : nullptr;
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
    // Full structure edits must wait for both the load transaction and secondary
    // GPU streaming. Scene switching uses the narrower isSceneSwitchBusy below.
    if (const SceneViewState* vs = app.tryResource<SceneViewState>(); vs && vs->loadSession.isBusy())
        return true;
    ::SceneManager* manager = sessionManager(app);
    return manager && manager->isStructureEditInFlight();
}

bool isSceneSwitchBusy(const App& app)
{
    // Secondary streaming (OMM / opacity) belongs to the currently displayed
    // scene and is safely cancelled by the normal scene teardown transaction.
    if (const SceneViewState* vs = app.tryResource<SceneViewState>();
        vs && vs->loadSession.isActive())
        return true;
    ::SceneManager* manager = sessionManager(app);
    return manager && manager->isSceneStructureBusy();
}

bool shouldSkipRender(const App& app)
{
    return activeScene(app) == nullptr;
}

bool isSceneLoading(const App& app)
{
    // Prefer LoadSession (Importing / GpuStreaming / Teardown / OMM secondary).
    // SceneManager::isSceneLoading is Importing-only (SceneLoader task).
    if (const SceneViewState* vs = app.tryResource<SceneViewState>(); vs && vs->loadSession.isBusy())
        return true;
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
    // Pick id is StandardMaterial::gpuDataIndex (path-tracer / Material Editor).
    // Do not fall back to dense Material::materialID — it can diverge after imports.
    if (materialID < 0)
        return nullptr;

    auto* wr = worldRenderer(app);
    const auto& cache = wr ? wr->lightingPasses().materials() : nullptr;
    const std::shared_ptr<StandardMaterial> standardFromCache =
        cache ? cache->findByGpuDataIndex(uint(materialID)) : nullptr;

    if (!standardFromCache)
        return nullptr;

    // Prefer a live scene MaterialEx so the editor keeps a stable identity;
    // re-link standardData for StandardMaterial::safeCast / Material Editor.
    if (const std::shared_ptr<Scene> active = activeScene(app))
    {
        for (const auto& mat : active->getMaterials())
        {
            auto materialEx = std::dynamic_pointer_cast<MaterialEx>(mat);
            if (!materialEx || !mat)
                continue;
            if (cache->findByResourceId(RenderResourceAccess::materialId(mat.get())).get()
                != standardFromCache.get())
                continue;
            materialEx->standardData = standardFromCache;
            return mat;
        }
    }

    // No scene counterpart — wrap for Material Editor only.
    auto wrap = std::make_shared<MaterialEx>();
    wrap->standardData = standardFromCache;
    wrap->name = standardFromCache->name;
    wrap->modelFileName = standardFromCache->modelName;
    return wrap;
}

std::shared_ptr<Material> linkRuntimeMaterialData(
    const App& app,
    const std::shared_ptr<Material>& material)
{
    const std::shared_ptr<MaterialEx> materialEx =
        material ? std::dynamic_pointer_cast<MaterialEx>(material) : nullptr;
    if (!materialEx || materialEx->standardData)
        return material;

    auto* wr = worldRenderer(app);
    const auto& cache = wr ? wr->lightingPasses().materials() : nullptr;
    if (!cache)
        return material;

    materialEx->standardData = cache->findByResourceId(
        RenderResourceAccess::materialId(materialEx.get()));
    return material;
}

ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex)
{
    return SceneManager::findEntityByInstanceIndex(activeScene(app), instanceIndex);
}

ecs::Entity findEntity(
    const App& app,
    const std::filesystem::path& path,
    ecs::Entity context)
{
    scene::SceneEntityWorld* world = entityWorld(app);
    if (!world)
        return ecs::NullEntity;
    if (!ecs::isValid(context))
        context = world->root();
    return world->findEntity(path, context);
}

SceneLoadStatus sceneLoadStatus(const App& app)
{
    SceneLoadStatus out;
    SceneViewState* vs = viewState(app);
    if (!vs)
        return out;

    const LoadSession& session = vs->loadSession;
    out.phaseName = loadSessionPhaseName(session.phase);
    out.busy = session.isBusy();
    out.progressPercent = session.progressPercent();
    out.gpuStreaming = session.phase == LoadSessionPhase::GpuStreaming;
    out.streamStep = static_cast<uint32_t>(session.streamStep);
    out.texturesRemaining = session.stepTexturesRemaining.load(std::memory_order_relaxed);
    out.meshBegin = session.meshBegin;
    out.meshTotal = session.meshTotal;
    out.stepInFlight = session.stepInFlight;
    return out;
}

const GameSettings* gameSettings(const App& app)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    return scene ? scene->getGameSettings() : nullptr;
}

const std::vector<SceneImportResult>& importedModels(const App& app)
{
    static const std::vector<SceneImportResult> kEmpty;
    const std::shared_ptr<Scene> scene = activeScene(app);
    return scene ? scene->getModels() : kEmpty;
}

std::shared_ptr<SceneTypeFactory> sceneTypeFactory(const App& app)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    return scene ? scene->getSceneTypeFactory() : nullptr;
}

} // namespace caustica
