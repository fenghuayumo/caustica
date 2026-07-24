#include <engine/SceneMeshEdit.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneMeshEditing.h>
#include <engine/SceneQuery.h>
#include <backend/GpuDevice.h>
#include <render/WorldRenderer.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>

namespace caustica
{
namespace
{

SetSceneMeshVerticesParams makeInternalMeshEditParams(App& app, const SceneMeshEditOptions& options)
{
    SetSceneMeshVerticesParams params;
    params.recomputeNormals = options.recomputeNormals;
    params.rebuildAccelerationStructure = options.rebuildAccelerationStructure;
    params.zeroMotionHistory = options.zeroMotionHistory;

    GpuDevice* device = gpuDevice(app);
    if (!device || !device->getDevice())
        return params;

    params.device = device->getDevice();
    params.frameIndex = device->getFrameIndex();
    params.scene = activeScene(app);

    if (GpuSharedCaches* caches = gpuSharedCaches(app))
        params.descriptorTable = caches->descriptorTable.get();

    if (render::WorldRenderer* wr = worldRenderer(app))
        params.gpuResources = &wr->sceneGpuResources();

    if (PathTracerSettings* pathSettings = settings(app))
        params.resetAccumulation = &pathSettings->ResetAccumulation;

    const bool resetAccumOnRebuild = options.resetAccumulationOnAccelRebuild;
    params.requestMeshAccelRebuild =
        [&app, resetAccumOnRebuild](const std::shared_ptr<MeshInfo>& dirtyMesh) {
            requestMeshAccelRebuild(app, dirtyMesh, resetAccumOnRebuild);
        };
    return params;
}

} // namespace

std::vector<dm::float3> getMeshVertices(App& /*app*/, const std::shared_ptr<MeshInfo>& mesh)
{
    return caustica::getMeshVertices(mesh);
}

std::vector<dm::float3> getMeshVerticesWorld(App& app, const std::shared_ptr<MeshInfo>& mesh)
{
    GpuDevice* device = gpuDevice(app);
    const uint32_t frameIndex = device ? device->getFrameIndex() : 0u;
    return caustica::getMeshVerticesWorld(activeScene(app), mesh, frameIndex);
}

std::vector<dm::float3> getMeshVerticesWorld(App& app, ecs::Entity entity)
{
    GpuDevice* device = gpuDevice(app);
    const uint32_t frameIndex = device ? device->getFrameIndex() : 0u;
    return caustica::getMeshVerticesWorld(activeScene(app), entity, frameIndex);
}

void setMeshVertices(
    App& app,
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options)
{
    caustica::setMeshVertices(mesh, vertices, makeInternalMeshEditParams(app, options));
}

void setMeshVerticesWorld(
    App& app,
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options)
{
    caustica::setMeshVerticesWorld(mesh, vertices, makeInternalMeshEditParams(app, options));
}

void setMeshVerticesWorld(
    App& app,
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options)
{
    caustica::setMeshVerticesWorld(entity, vertices, makeInternalMeshEditParams(app, options));
}

bool applyGeometrySequence(
    App& app,
    ecs::Entity entity,
    float timeSeconds,
    const SceneMeshEditOptions& options)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    scene::SceneEntityWorld* ew = scene ? scene->getEntityWorld() : nullptr;
    if (!ew || !ecs::isValid(entity))
        return false;
    auto* sequence = ew->world().tryGet<scene::GeometrySequenceComponent>(entity);
    if (!sequence)
        return false;
    return caustica::applyGeometrySequence(*sequence, timeSeconds, makeInternalMeshEditParams(app, options));
}

} // namespace caustica
