#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SceneGaussianSplatLogic.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneSpawn.h>
#include <engine/SceneQuery.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderSessionApi.h>
#include <engine/ScenePlugins.h>
#include <assets/AssetSystem.h>
#include <assets/RuntimeMeshLoadTypes.h>
#include <scene/loader/RuntimeMeshLoader.h>
#include <scene/SceneApply.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/SceneRayTracingResources.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/WorldRenderer.h>
#include <assets/loader/TextureLoader.h>
#include <assets/loader/ShaderMacro.h>
#include <backend/GpuDevice.h>

using namespace caustica::render;

namespace caustica
{

namespace
{

void runStructureGpuBuild(
    render::WorldRenderer* worldRendererResource,
    GpuSharedCaches* caches,
    const std::shared_ptr<Scene>& scenePtr,
    GpuDevice* device,
    uint32_t frameIndex,
    const std::shared_ptr<const scene::SceneRenderData>& gpuSetupData)
{
    if (caches->textureLoader && caches->renderDevice)
    {
        caches->textureLoader->processRenderingThreadCommands(*caches->renderDevice, 0.f);
        caches->textureLoader->loadingFinished();
    }

    worldRendererResource->lightingPasses().ensureMaterialsFromScene(*gpuSetupData);
    // Do not prune mesh/material GPU records still referenced by the retired TLAS.
    render::SceneGpuUpdater::refreshAfterLoad(
        *scenePtr,
        *gpuSetupData,
        worldRendererResource->sceneGpuResources(),
        caches->descriptorTable.get(),
        frameIndex,
        /*pruneRemovedResources=*/false);

    // Double-buffered AS rebuild: previous TLAS/BLAS stay alive for in-flight frames
    // while this task builds the new generation (overlaps prior-frame GPU work).
    worldRendererResource->rayTracingResources().requestAccelerationStructureRebuild();
    caustica::rhi::CommandListHandle commandList = device->getDevice()->createCommandList();
    worldRendererResource->rayTracingResources().recreateAccelStructs(
        commandList, *scenePtr, gpuSetupData.get());

    // SBT: rebuildShaderTableOnly allocates a new table; old tables stay referenced by
    // in-flight DispatchRays (no waitForIdle required).
    if (auto compiler = worldRendererResource->rayTracingResources().pathTracingShaderCompiler())
    {
        compiler->update(
            gpuSetupData.get(),
            static_cast<unsigned int>(worldRendererResource->accelStructs().getSubInstanceData().size()),
            [worldRendererResource](std::vector<caustica::ShaderMacro>& macros) {
                worldRendererResource->rayTracingResources().fillPTPipelineGlobalMacros(macros);
            },
            false);
    }

    worldRendererResource->rayTracingResources().recreateBindingSet(gpuSetupData.get());
    scenePtr->finishStructureGpuBuild(frameIndex, gpuSetupData);
}

} // namespace

bool enqueuePendingStructureGpu(App& app)
{
    render::WorldRenderer* worldRendererResource = worldRenderer(app);
    GpuSharedCaches* caches = gpuSharedCaches(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!worldRendererResource || !caches || !device || !scenePtr || !scenePtr->needsGpuStructureSync())
        return false;

    // Coalesce: one in-flight structure build at a time. Keep pending so Extract retries.
    if (scenePtr->structureGpuBuildInFlight())
        return false;

    const uint32_t frameIndex = device->getPreparedRenderFrameIndex();
    assert(scenePtr->wasRenderSnapshotExtractedOnLogicThread(frameIndex));

    // Copy the published packet — the triple-buffer slot may be reused before RT runs.
    auto gpuSetupData = std::make_shared<const scene::SceneRenderData>(
        scenePtr->getRenderDataForFrame(frameIndex));

    scenePtr->beginStructureGpuBuild();
    scenePtr->clearGpuStructureSyncRequest();

    enqueueGpuWorkOnRenderThread(
        app,
        [worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData]() {
            runStructureGpuBuild(
                worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData);
        });

    return true;
}

void flushPendingStructureGpuSync(App& app)
{
    render::WorldRenderer* worldRendererResource = worldRenderer(app);
    GpuSharedCaches* caches = gpuSharedCaches(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!worldRendererResource || !caches || !device || !scenePtr || !scenePtr->needsGpuStructureSync())
        return;

    if (scenePtr->structureGpuBuildInFlight())
        return;

    const uint32_t frameIndex = device->getPreparedRenderFrameIndex();
    assert(scenePtr->wasRenderSnapshotExtractedOnLogicThread(frameIndex));

    auto gpuSetupData = std::make_shared<const scene::SceneRenderData>(
        scenePtr->getRenderDataForFrame(frameIndex));

    scenePtr->beginStructureGpuBuild();
    scenePtr->clearGpuStructureSyncRequest();

    runGpuWorkOnRenderThread(app, [worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData]() {
        runStructureGpuBuild(
            worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData);
    });
}

Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    GpuSharedCaches* caches = gpuSharedCaches(app);
    if (!assets || !caches || path.empty())
        return {};

    if (Handle<ScenePrefabAsset> existing = assets->findScenePrefab(path))
        return existing;

    auto textureLoader = caches->textureLoader;
    if (!textureLoader)
        return {};

    RuntimeMeshLoadParams params{
        .TextureCache = textureLoader.get(),
        .SceneTypes = std::make_shared<render::RenderSceneTypeFactory>(),
        .TextureSearchDirectory = path.parent_path(),
    };

    const RuntimeMeshLoadResult result = loadRuntimeMeshFile(params, path);
    if (!result)
        return {};

    return assets->registerScenePrefab(result.ImportResult, path);
}

ecs::Entity spawn(App& app, const Handle<ScenePrefabAsset>& prefab, const SceneApplyCallbacks& callbacks)
{
    if (!prefab || !prefab->import)
        return ecs::NullEntity;

    ::SceneManager* manager = detail::sessionManager(app);
    auto scenePtr = activeScene(app);
    if (!manager || !scenePtr)
        return ecs::NullEntity;

    if (!manager->tryBeginStructureEdit())
        return ecs::NullEntity;

    struct StructureEditGuard
    {
        ::SceneManager* manager = nullptr;
        ~StructureEditGuard()
        {
            if (manager)
                manager->endStructureEdit();
        }
    } guard{ manager };

    // ECS mutation is logic-thread only (assertLogicThread on getEntityWorld).
    // GPU/AS work is enqueued from Extract; no RT idle stall on spawn.
    const ecs::Entity root = attachImportedScene(scenePtr, *prefab->import, callbacks);
    if (!ecs::isValid(root))
        return ecs::NullEntity;

    return root;
}

ecs::Entity spawnFromFile(
    App& app,
    const std::filesystem::path& path,
    const SceneApplyCallbacks& callbacks)
{
    return spawn(app, load(app, path), callbacks);
}

bool despawn(App& app, ecs::Entity entity)
{
    ::SceneManager* manager = detail::sessionManager(app);
    render::WorldRenderer* worldRendererResource = worldRenderer(app);
    auto scenePtr = activeScene(app);
    if (!manager || !worldRendererResource || !scenePtr || !ecs::isValid(entity))
        return false;

    if (!manager->tryBeginStructureEdit())
        return false;

    struct StructureEditGuard
    {
        ::SceneManager* manager = nullptr;
        ~StructureEditGuard()
        {
            if (manager)
                manager->endStructureEdit();
        }
    } guard{ manager };

    if (!destroySceneEntity(DestroySceneEntityParams{
            .scene = scenePtr,
            .entity = entity,
            .beforeDetach = [worldRendererResource](ecs::Entity deletedEntity) {
                SceneGaussianSplatLogic::removeObjectsUnderEntity(
                    worldRendererResource->gaussianSplatPasses(), deletedEntity);
            },
        }))
    {
        return false;
    }

    return true;
}

} // namespace caustica
