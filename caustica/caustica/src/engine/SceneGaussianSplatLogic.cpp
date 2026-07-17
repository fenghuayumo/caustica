#include <engine/SceneGaussianSplatLogic.h>
#include <render/SceneGaussianSplatPasses.h>

#include <backend/GpuDevice.h>
#include <core/ThreadContext.h>
#include <core/log.h>
#include <render/PathTracerScenePasses.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <unordered_set>

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

void SceneGaussianSplatLogic::onSceneLoaded(SceneGaussianSplatPasses& passes)
{
    assertLogicThread();
    if (passes.m_gpuDevice)
        passes.m_gpuDevice->waitForRenderThreadIdle();
    loadFromSceneEntities(passes);
}

bool SceneGaussianSplatLogic::loadFromFile(
    SceneGaussianSplatPasses& passes,
    const std::filesystem::path& fileName,
    bool convertRdfToRub)
{
    assertLogicThread();
    if (passes.m_gpuDevice)
        passes.m_gpuDevice->waitForRenderThreadIdle();
    return attachToScene(passes, fileName, convertRdfToRub);
}

bool SceneGaussianSplatLogic::removeObjectsUnderEntity(
    SceneGaussianSplatPasses& passes,
    ecs::Entity rootEntity)
{
    assertLogicThread();
    if (passes.m_gpuDevice)
        passes.m_gpuDevice->waitForRenderThreadIdle();

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

void SceneGaussianSplatLogic::loadFromSceneEntities(SceneGaussianSplatPasses& passes)
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
                    "Gaussian Splat entity '%s' has no path/file field.",
                    entityWorld->getEntityName(entity).c_str());
                return;
            }

            auto pass = std::make_shared<GaussianSplatPass>(
                passes.m_gpuDevice->getDevice(), passes.m_shaderFactory);
            if (pass->loadFromFile(splatPath, splat.convertRdfToRub))
            {
                splat.resolvedPath = splatPath.string();
                splat.loadedSplatCount = pass->getSplatCount();
                passes.onPassLoaded(*pass);
                ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);

                SceneGaussianSplatPasses::SceneObject object;
                object.entity = entity;
                object.pass = std::move(pass);
                passes.m_objects.push_back(std::move(object));
            }
            else
            {
                caustica::error(
                    "Failed to load Gaussian Splat entity '%s' from '%s'.",
                    entityWorld->getEntityName(entity).c_str(),
                    splatPath.string().c_str());
            }
        });

    passes.updateUIState();
    if (passes.m_onTemporalReset)
        passes.m_onTemporalReset();
}

bool SceneGaussianSplatLogic::attachToScene(
    SceneGaussianSplatPasses& passes,
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
        double(passes.m_settings->GaussianSplatTranslation.x),
        double(passes.m_settings->GaussianSplatTranslation.y),
        double(passes.m_settings->GaussianSplatTranslation.z)));
    entityWorld->setRotation(entity, dm::rotationQuat(dm::double3(
        double(passes.m_settings->GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(passes.m_settings->GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(passes.m_settings->GaussianSplatRotationEulerDeg.z) * deg2rad)));
    entityWorld->setScaling(entity, dm::double3(
        double(passes.m_settings->GaussianSplatObjectScale.x),
        double(passes.m_settings->GaussianSplatObjectScale.y),
        double(passes.m_settings->GaussianSplatObjectScale.z)));
    entityWorld->setGaussianSplat(entity, splat);

    passes.onPassLoaded(*pass);
    ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);
    scene->requestGpuStructureSync();

    SceneGaussianSplatPasses::SceneObject object;
    object.entity = entity;
    object.pass = std::move(pass);
    passes.m_objects.push_back(std::move(object));

    passes.m_settings->EnableGaussianSplats = true;
    passes.updateUIState();
    if (passes.m_onTemporalReset)
        passes.m_onTemporalReset();
    if (passes.m_onRequestFullRebuild)
        passes.m_onRequestFullRebuild();
    return true;
}

} // namespace caustica
