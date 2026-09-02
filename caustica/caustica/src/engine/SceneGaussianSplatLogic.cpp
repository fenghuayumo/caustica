#include <engine/SceneGaussianSplatLogic.h>
#include <engine/App.h>
#include <render/SceneGaussianSplatPasses.h>

#include <backend/GpuDevice.h>
#include <core/ThreadContext.h>
#include <core/log.h>
#include <render/PathTracerScenePasses.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace caustica
{

using namespace render;

namespace
{

std::string MakeUniqueChildEntityName(
    const scene::SceneEntityWorld& entityWorld,
    ecs::Entity parent,
    const std::string& desiredName)
{
    const std::string baseName = desiredName.empty() ? "GaussianSplat" : desiredName;

    std::unordered_set<std::string> existingNames;
    for (ecs::Entity child : entityWorld.getEntityChildren(parent))
        existingNames.insert(entityWorld.getEntityName(child));

    if (!existingNames.contains(baseName))
        return baseName;

    for (uint32_t suffix = 2; ; ++suffix)
    {
        std::string candidate = baseName + " (" + std::to_string(suffix) + ")";
        if (!existingNames.contains(candidate))
            return candidate;
    }
}

void ApplyGaussianSplatLocalBounds(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity,
    const GaussianSplatPass& pass)
{
    entityWorld.world().emplace<scene::LocalBoundsComponent>(
        entity, scene::LocalBoundsComponent{ pass.getLocalBounds() });
    entityWorld.refreshHierarchy(scene::PreviousTransformPolicy::PreserveExisting);
}

} // namespace

void SceneGaussianSplatLogic::onSceneLoaded(
    SceneGaussianSplatPasses& passes,
    PathTracerSettings& settings,
    App& app)
{
    assertLogicThread();
    // THREADING: Logic↔RT wait — ADR 0002 S5 remaining (Pass create still on Logic;
    // move GaussianSplatPass factory to RT before removing this drain).
    app.waitForRenderThreadIdle();
    loadFromSceneEntities(passes, settings);
}

bool SceneGaussianSplatLogic::loadFromFile(
    SceneGaussianSplatPasses& passes,
    PathTracerSettings& settings,
    const std::filesystem::path& fileName,
    App& app,
    bool convertRdfToRub)
{
    assertLogicThread();
    // THREADING: Logic↔RT wait — ADR 0002 S5 remaining (Pass create still on Logic).
    app.waitForRenderThreadIdle();
    return attachToScene(passes, settings, fileName, convertRdfToRub);
}

bool SceneGaussianSplatLogic::removeObjectsUnderEntity(
    SceneGaussianSplatPasses& passes,
    ecs::Entity rootEntity,
    App& app)
{
    assertLogicThread();
    // THREADING: Logic↔RT wait — ADR 0002 S5 remaining (Pass shared_ptr drop vs RT use).
    app.waitForRenderThreadIdle();

    if (!ecs::isValid(rootEntity))
        return false;

    const scene::SceneEntityWorld* entityWorld = passes.m_sessionScene
        ? passes.m_sessionScene->getEntityWorld()
        : nullptr;
    if (!entityWorld)
        return false;

    bool removedGaussianSplat = false;
    auto removedBegin = std::remove_if(
        passes.m_objects.begin(),
        passes.m_objects.end(),
        [&](const SceneGaussianSplatPasses::SceneObject& object)
        {
            const bool remove = ecs::isValid(object.entity)
                && entityWorld->entitySubtreeContains(rootEntity, object.entity);
            removedGaussianSplat = removedGaussianSplat || remove;
            return remove;
        });
    if (removedBegin != passes.m_objects.end())
        passes.m_objects.erase(removedBegin, passes.m_objects.end());

    if (removedGaussianSplat)
    {
        passes.updateUIState();
        if (passes.m_onTemporalReset)
            passes.m_onTemporalReset();
    }

    return removedGaussianSplat;
}

void SceneGaussianSplatLogic::loadFromSceneEntities(
    SceneGaussianSplatPasses& passes,
    PathTracerSettings& settings)
{
    assertLogicThread();
    passes.m_objects.clear();

    if (!passes.m_sessionScene
        || !passes.m_sessionScene->getEntityWorld()
        || !passes.m_shaderFactory)
    {
        passes.updateUIState();
        return;
    }

    auto* entityWorld = passes.m_sessionScene->getEntityWorld();
    // Destroy after iteration — despawning mid-each<> is unsafe.
    std::vector<ecs::Entity> failedEntities;

    entityWorld->world().each<scene::GaussianSplatComponent>(
        [&](ecs::Entity entity, scene::GaussianSplatComponent& component)
        {
            GaussianSplat& splat = component.splat;
            splat.loadedSplatCount = 0;
            splat.resolvedPath.clear();

            const std::filesystem::path splatPath = passes.resolveSplatPath(splat);
            if (splatPath.empty())
            {
                caustica::error(
                    "Gaussian Splat entity '%s' has no path/file field; removing entity.",
                    entityWorld->getEntityName(entity).c_str());
                failedEntities.push_back(entity);
                return;
            }

            auto pass = std::make_shared<GaussianSplatPass>(
                passes.m_gpuDevice->getDevice(), passes.m_shaderFactory);
            if (pass->loadFromFile(splatPath, splat.convertRdfToRub)
                && pass->getSplatCount() > 0)
            {
                splat.resolvedPath = splatPath.string();
                splat.loadedSplatCount = pass->getSplatCount();
                ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);

                SceneGaussianSplatPasses::SceneObject object;
                object.entity = entity;
                object.pass = std::move(pass);
                passes.m_objects.push_back(std::move(object));
                return;
            }

            caustica::error(
                "Failed to load Gaussian Splat entity '%s' from '%s'; removing entity.",
                entityWorld->getEntityName(entity).c_str(),
                splatPath.string().c_str());
            failedEntities.push_back(entity);
        });

    for (ecs::Entity entity : failedEntities)
        entityWorld->destroyEntity(entity);

    // The vk reference renders raster splats at the display resolution with DLSS
    // disabled. Do the same when a scene containing splats is opened; users can
    // explicitly re-enable TAA/DLSS afterwards when they prefer performance.
    if (!passes.m_objects.empty() && settings.RealtimeAA != 0)
    {
        caustica::info("3D Gaussian Splats: using native-resolution rendering for reference quality.");
        settings.RealtimeAA = 0;
    }

    // Do not requestGpuStructureSync here. Failed splats never contributed GPU
    // resources, and during onSceneLoaded materials/AS may not exist yet —
    // early structure sync would crash in recreateBindingSet. Hierarchy reads
    // live ECS, so destroyEntity is enough; the next extract picks up the change.

    passes.updateUIState();
    if (passes.m_onTemporalReset)
        passes.m_onTemporalReset();
}

bool SceneGaussianSplatLogic::attachToScene(
    SceneGaussianSplatPasses& passes,
    PathTracerSettings& settings,
    const std::filesystem::path& fileName,
    bool convertRdfToRub)
{
    assertLogicThread();

    caustica::Scene* scene = passes.m_sessionScene;
    auto* entityWorld = scene ? scene->getEntityWorld() : nullptr;
    if (!scene || !entityWorld || !ecs::isValid(entityWorld->root()))
    {
        caustica::error("Cannot load Gaussian splats before a scene is loaded.");
        return false;
    }
    if (!passes.m_shaderFactory)
    {
        caustica::error("Cannot load Gaussian splats before the shader factory is initialized.");
        return false;
    }

    std::filesystem::path splatPath = fileName;
    if (!splatPath.is_absolute())
        splatPath = std::filesystem::absolute(splatPath);

    if (!std::filesystem::exists(splatPath))
    {
        caustica::error(
            "Gaussian Splat file does not exist: '%s'",
            splatPath.string().c_str());
        return false;
    }

    auto pass = std::make_shared<GaussianSplatPass>(
        passes.m_gpuDevice->getDevice(), passes.m_shaderFactory);
    if (!pass->loadFromFile(splatPath, convertRdfToRub))
    {
        caustica::error(
            "Failed to load Gaussian Splat file '%s'.",
            splatPath.string().c_str());
        return false;
    }
    if (pass->getSplatCount() == 0)
    {
        caustica::error(
            "Gaussian Splat file '%s' contains no splats.",
            splatPath.string().c_str());
        return false;
    }

    GaussianSplat splat;
    splat.path = splatPath.string();
    splat.resolvedPath = splatPath.string();
    splat.convertRdfToRub = convertRdfToRub;
    splat.enabled = true;
    splat.loadedSplatCount = pass->getSplatCount();

    const ecs::Entity parent = entityWorld->root();
    const std::string entityName = MakeUniqueChildEntityName(
        *entityWorld, parent, splatPath.filename().string());
    ecs::Entity entity = entityWorld->createEntity(entityName, parent);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    entityWorld->setTranslation(entity, dm::double3(
        double(settings.GaussianSplatTranslation.x),
        double(settings.GaussianSplatTranslation.y),
        double(settings.GaussianSplatTranslation.z)));
    entityWorld->setRotation(entity, dm::rotationQuat(dm::double3(
        double(settings.GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(settings.GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(settings.GaussianSplatRotationEulerDeg.z) * deg2rad)));
    entityWorld->setScaling(entity, dm::double3(
        double(settings.GaussianSplatObjectScale.x),
        double(settings.GaussianSplatObjectScale.y),
        double(settings.GaussianSplatObjectScale.z)));
    entityWorld->setGaussianSplat(entity, splat);

    ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);
    scene->requestGpuStructureSync();

    SceneGaussianSplatPasses::SceneObject object;
    object.entity = entity;
    object.pass = std::move(pass);
    passes.m_objects.push_back(std::move(object));

    settings.EnableGaussianSplats = true;
    if (settings.RealtimeAA != 0)
    {
        caustica::info("3D Gaussian Splats: disabling TAA/DLSS to use native-resolution reference quality.");
        settings.RealtimeAA = 0;
    }
    passes.updateUIState();
    if (passes.m_onTemporalReset)
        passes.m_onTemporalReset();
    if (passes.m_onRequestFullRebuild)
        passes.m_onRequestFullRebuild();
    return true;
}

} // namespace caustica
