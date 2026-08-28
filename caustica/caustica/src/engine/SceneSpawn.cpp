#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SceneGaussianSplatLogic.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneSpawn.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/RenderSessionApi.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/ScenePlugins.h>
#include <assets/AssetSystem.h>
#include <assets/RuntimeMeshLoadTypes.h>
#include <scene/loader/RuntimeMeshLoader.h>
#include <scene/SceneApply.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>
#include <scene/SceneImport.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/SceneRayTracingResources.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/WorldRenderer.h>
#include <assets/loader/TextureLoader.h>
#include <assets/loader/ShaderMacro.h>
#include <core/path_utils.h>
#include <backend/GpuDevice.h>

using namespace caustica::render;

namespace caustica
{

namespace
{

bool runStructureGpuBuild(
    render::WorldRenderer* worldRendererResource,
    GpuSharedCaches* caches,
    const std::shared_ptr<Scene>& scenePtr,
    GpuDevice* device,
    uint32_t frameIndex,
    const std::shared_ptr<const scene::SceneRenderData>& gpuSetupData,
    StructureGpuUploadMode uploadMode,
    bool waitForCompletion)
{
    if (!worldRendererResource || !caches || !scenePtr || !device || !gpuSetupData)
        return false;
    caustica::rhi::Device* rhiDevice = device->getDevice();
    if (!rhiDevice || !rhiDevice->isDeviceHealthy())
        return false;

    if (uploadMode == StructureGpuUploadMode::UploadMeshes)
    {
        if (caches->textureLoader && caches->renderDevice)
        {
            caches->textureLoader->processRenderingThreadCommands(*caches->renderDevice, 0.f);
            caches->textureLoader->loadingFinished();
        }

        worldRendererResource->lightingPasses().ensureMaterialsFromScene(*gpuSetupData);
        // Do not prune mesh/material GPU records still referenced by the retired TLAS.
        if (!render::SceneGpuUpdater::refreshAfterLoad(
            *scenePtr,
            *gpuSetupData,
            worldRendererResource->sceneGpuResources(),
            caches->descriptorTable.get(),
            frameIndex,
            /*pruneRemovedResources=*/false))
            return false;
    }
    // AccelOnly: LoadSession already flushed textures, uploaded meshes, finalized buffers,
    // and reloaded materials — only AS / SBT / bindings remain.

    // Double-buffered AS rebuild: previous TLAS/BLAS stay alive for in-flight frames
    // while this task builds the new generation (overlaps prior-frame GPU work).
    worldRendererResource->rayTracingResources().requestAccelerationStructureRebuild();
    detail::sceneSwitchTrace("StructureGpu: begin acceleration structures");
    if (waitForCompletion)
    {
        if (!worldRendererResource->rayTracingResources().recreateAccelStructsForLoad(
            *scenePtr,
            *gpuSetupData,
            gpuSetupData->renderSettings.settings,
            256ull * 1024ull * 1024ull,
            [](const char* stage, size_t completed, size_t total, uint64_t scratchBytes) {
                detail::sceneSwitchTrace(
                    "StructureGpu: AS stage=%s meshes=%zu/%zu scratch=%llu",
                    stage,
                    completed,
                    total,
                    static_cast<unsigned long long>(scratchBytes));
            }))
            return false;
    }
    else
    {
        caustica::rhi::CommandListHandle commandList = rhiDevice->createCommandList();
        if (!worldRendererResource->rayTracingResources().recreateAccelStructs(
            commandList,
            *scenePtr,
            gpuSetupData->renderSettings.settings,
            gpuSetupData.get()))
            return false;
    }
    detail::sceneSwitchTrace("StructureGpu: acceleration structures complete");

    // Runtime structure edits can refresh SBT/bindings immediately because the
    // renderer is already initialized. During exclusive scene load those
    // resources are deliberately owned by the first renderer-init frame: cold
    // startup has no compiler/environment/light-sampling objects yet, and scene
    // unload clears the scene-scoped lighting resources.
    if (!waitForCompletion)
    {
        // SBT: rebuildShaderTableOnly allocates a new table; old tables stay
        // referenced by in-flight DispatchRays (no waitForIdle required).
        auto& rayTracing = worldRendererResource->rayTracingResources();
        if (rayTracing.hasPipelineRuntime())
        {
            detail::sceneSwitchTrace("StructureGpu: begin SBT/pipeline update");
            rayTracing.updatePipelineRuntime(
                gpuSetupData.get(),
                static_cast<unsigned int>(worldRendererResource->accelStructs().getSubInstanceData().size()),
                false,
                gpuSetupData->renderSettings.settings);
            detail::sceneSwitchTrace("StructureGpu: SBT/pipeline update complete");
        }

        detail::sceneSwitchTrace("StructureGpu: begin binding set");
        worldRendererResource->recreateBindingSet(gpuSetupData.get());
        if (!worldRendererResource->hasSceneBindingSet() || !rhiDevice->isDeviceHealthy())
            return false;
        detail::sceneSwitchTrace("StructureGpu: binding set complete");
    }
    else
    {
        detail::sceneSwitchTrace("StructureGpu: defer SBT/binding to renderer-init frame");
    }

    // The exclusive load AS scheduler has already waited its own graphics
    // fences. A device-wide idle here can wait on unrelated frame/present work
    // owned by this render thread and deadlock startup.
    detail::sceneSwitchTrace("StructureGpu: transaction complete");

    scenePtr->finishStructureGpuBuild(frameIndex, gpuSetupData);
    return true;
}

} // namespace

bool buildSceneGpuStructure(
    App& app,
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<const scene::SceneRenderData>& renderData,
    StructureGpuUploadMode uploadMode,
    uint32_t frameIndex,
    bool waitForCompletion)
{
    return runStructureGpuBuild(
        worldRenderer(app),
        gpuSharedCaches(app),
        scene,
        gpuDevice(app),
        frameIndex,
        renderData,
        uploadMode,
        waitForCompletion);
}

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
    const StructureGpuUploadMode uploadMode = scenePtr->structureGpuUploadMode();

    scenePtr->beginStructureGpuBuild();
    scenePtr->clearGpuStructureSyncRequest();

    EnqueueRenderCommand(
        app,
        [worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData, uploadMode]() {
            if (!runStructureGpuBuild(
                worldRendererResource, caches, scenePtr, device, frameIndex, gpuSetupData, uploadMode,
                /*waitForCompletion=*/false))
            {
                caustica::error("Asynchronous scene structure GPU build failed");
                scenePtr->failStructureGpuBuild();
            }
        });

    return true;
}

Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    GpuSharedCaches* caches = gpuSharedCaches(app);
    if (!assets || !caches || path.empty())
        return {};

    std::filesystem::path resolved = path;
    if (!resolved.is_absolute() && !std::filesystem::exists(resolved))
        resolved = resolveSceneMediaPath(path, {});

    if (Handle<ScenePrefabAsset> existing = assets->findScenePrefab(resolved))
        return existing;

    if (isPrefabAssetPath(path.generic_string()) || isPrefabAssetPath(resolved.generic_string()))
    {
        auto scenePtr = activeScene(app);
        if (!scenePtr)
            return {};

        auto imported = std::make_shared<SceneImportResult>(
            scenePtr->loadOrGetPrefab(path.generic_string(), /*asyncTextures=*/true));
        if (!imported->entityWorld || !ecs::isValid(imported->rootEntity))
            return {};
        return assets->registerScenePrefab(imported, resolved);
    }

    auto textureLoader = caches->textureLoader;
    if (!textureLoader)
        return {};

    RuntimeMeshLoadParams params{
        .TextureCache = textureLoader.get(),
        .SceneTypes = std::make_shared<render::RenderSceneTypeFactory>(),
        .TextureSearchDirectory = resolved.parent_path(),
    };

    const RuntimeMeshLoadResult result = loadRuntimeMeshFile(params, resolved);
    if (!result)
        return {};

    return assets->registerScenePrefab(result.ImportResult, resolved);
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
