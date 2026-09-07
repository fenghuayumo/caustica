#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <rhi/rhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace caustica
{

class IDescriptorTableManager;
class Scene;
namespace render { struct SceneGpuResources; }
struct MeshInfo;

namespace scene
{
struct GeometrySequenceComponent;
}

// Engine-internal GPU wiring for mesh uploads. Applications must not include this.
// Call MeshDeformApi.h (prefer entity / MeshHandle paths) instead.
struct MeshDeformGpuParams
{
    caustica::rhi::Device* device = nullptr;
    IDescriptorTableManager* descriptorTable = nullptr;
    render::SceneGpuResources* gpuResources = nullptr;
    std::shared_ptr<Scene> scene;
    uint32_t frameIndex = 0;
    bool recomputeNormals = true;
    bool rebuildAccelerationStructure = true;
    // When true, write the new positions into both Position and PrevPosition so
    // motion vectors are zero (e.g. animation loop wrap / first sample).
    bool zeroMotionHistory = false;
    bool* resetAccumulation = nullptr;
    std::function<void(const std::shared_ptr<MeshInfo>&)> requestMeshAccelRebuild;
};

std::vector<math::float3> getMeshVertices(const std::shared_ptr<MeshInfo>& mesh);
std::vector<math::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    uint32_t frameIndex);
std::vector<math::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity entity,
    uint32_t frameIndex);

void setMeshVertices(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<math::float3>& vertices,
    const MeshDeformGpuParams& params);
void setMeshVerticesWorld(
    ecs::Entity entity,
    const std::vector<math::float3>& vertices,
    const MeshDeformGpuParams& params);
void setMeshVerticesWorld(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<math::float3>& vertices,
    const MeshDeformGpuParams& params);

// Direct 1:1 update of mesh->buffers->positionData[vertexOffset .. +count).
// Used by geometry-sequence playback (fixed topology point caches).
void setMeshPositionsDirect(
    const std::shared_ptr<MeshInfo>& mesh,
    const math::float3* positions,
    size_t count,
    const MeshDeformGpuParams& params);

// Sample a GeometrySequenceComponent at `timeSeconds` and upload positions.
bool applyGeometrySequence(
    scene::GeometrySequenceComponent& sequence,
    float timeSeconds,
    const MeshDeformGpuParams& params);

} // namespace caustica
