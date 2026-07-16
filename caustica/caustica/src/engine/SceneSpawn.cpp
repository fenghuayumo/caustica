#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneSpawn.h>
#include <engine/SceneQuery.h>
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
#include <assets/loader/TextureLoader.h>
#include <assets/loader/ShaderMacro.h>
#include <backend/GpuDevice.h>

using namespace caustica::render;

namespace caustica
{

void flushPendingStructureGpu(App& app)
{
    GpuRenderSubsystem* gr = gpuRender(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!gr || !device || !scenePtr || !scenePtr->needsGpuStructureSync())
        return;

    // Exclusive access: no in-flight Extract/render reading an incomplete structure.
    device->waitForRenderThreadIdle();

    // Use logic frame index (matches Extract after SetRenderFrameIndex; correct for
    // immediate flush from spawn/despawn in PostUpdate before Extract runs).
    const uint32_t frameIndex = device->getFrameIndex();
    runGpuWorkOnRenderThread(app, [gr, scenePtr, device, frameIndex]() {
        if (nvrhi::IDevice* nvrhiDevice = device->getDevice())
            nvrhiDevice->waitForIdle();

        if (auto textureLoader = gr->textureLoader())
        {
            textureLoader->processRenderingThreadCommands(gr->renderDevice(), 0.f);
            textureLoader->loadingFinished();
        }

        gr->lightingPasses().ensureMaterialsFromScene(scenePtr);
        render::SceneGpuUpdater::refreshAfterLoad(*scenePtr, frameIndex);

        // Rebuild BLAS/TLAS immediately while exclusive. Leaving this to the next
        // render frame races new proxies against a stale TLAS and crashes nvwgf2umx.
        gr->rayTracingResources().requestAccelerationStructureRebuild();
        nvrhi::CommandListHandle commandList = device->getDevice()->createCommandList();
        gr->rayTracingResources().recreateAccelStructs(commandList);

        // Keep SBT hit-group count in lockstep with the new TLAS while GPU is idle.
        // Otherwise the next DispatchRays can use new contribution indices against a
        // short/stale shader table (intermittent hang after drag-drop import).
        if (auto compiler = gr->rayTracingResources().pathTracingShaderCompiler())
        {
            compiler->update(
                scenePtr,
                static_cast<unsigned int>(gr->accelStructs().getSubInstanceData().size()),
                [gr](std::vector<caustica::ShaderMacro>& macros) {
                    gr->rayTracingResources().fillPTPipelineGlobalMacros(macros);
                },
                false);
        }

        // Binding set still points at the destroyed TLAS until the next frame otherwise.
        gr->rayTracingResources().recreateBindingSet();
    });

    scenePtr->clearGpuStructureSyncRequest();
}

Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path)
{
    AssetSystem* assets = app.tryResource<AssetSystem>();
    GpuRenderSubsystem* gr = gpuRender(app);
    if (!assets || !gr || path.empty())
        return {};

    if (Handle<ScenePrefabAsset> existing = assets->findScenePrefab(path))
        return existing;

    auto textureLoader = gr->textureLoader();
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

    ::SceneManager* manager = sceneManager(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!manager || !device || !scenePtr)
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

    // Some render paths still touch live ECS; never mutate structure while RT is in-flight.
    device->waitForRenderThreadIdle();

    const ecs::Entity root = attachImportedScene(scenePtr, *prefab->import, callbacks);
    if (!ecs::isValid(root))
        return ecs::NullEntity;

    // GPU upload / AS rebuild happens in Extract (PrepareRenderFrame).
    syncSceneAccess(app);
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
    ::SceneManager* manager = sceneManager(app);
    GpuRenderSubsystem* gr = gpuRender(app);
    GpuDevice* device = gpuDevice(app);
    auto scenePtr = activeScene(app);
    if (!manager || !gr || !device || !scenePtr || !ecs::isValid(entity))
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

    device->waitForRenderThreadIdle();

    if (!destroySceneEntity(DestroySceneEntityParams{
            .scene = scenePtr,
            .entity = entity,
            .beforeDetach = [gr](ecs::Entity deletedEntity) {
                gr->gaussianSplatPasses().removeObjectsUnderEntity(deletedEntity);
            },
        }))
    {
        return false;
    }

    // GPU upload / AS rebuild happens in Extract (PrepareRenderFrame).
    syncSceneAccess(app);
    return true;
}

} // namespace caustica
