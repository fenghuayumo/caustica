#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace caustica
{

class Scene;
struct MeshInfo;

struct SetSceneMeshVerticesParams
{
    nvrhi::IDevice* device = nullptr;
    std::shared_ptr<Scene> scene;
    uint32_t frameIndex = 0;
    bool recomputeNormals = true;
    bool rebuildAccelerationStructure = true;
    bool* resetAccumulation = nullptr;
    std::function<void(const std::shared_ptr<MeshInfo>&)> requestMeshAccelRebuild;
};

std::vector<dm::float3> getMeshVertices(const std::shared_ptr<MeshInfo>& mesh);
std::vector<dm::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    uint32_t frameIndex);
std::vector<dm::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity entity,
    uint32_t frameIndex);

void setMeshVertices(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params);
void setMeshVerticesWorld(
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params);
void setMeshVerticesWorld(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params);

} // namespace caustica
