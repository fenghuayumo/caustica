#pragma once

#include <ecs/Entity.h>
#include <math/math.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace caustica
{

class App;
struct MeshInfo;

// App-facing mesh deform options. GPU upload / AS rebuild stay inside the engine.
// (Low-level GPU wiring lives in SceneMeshEditing.h — apps should not fill that.)
struct SceneMeshEditOptions
{
    bool recomputeNormals = true;
    bool rebuildAccelerationStructure = true;
    // Write Position and PrevPosition so motion vectors are zero (loop wrap / first sample).
    bool zeroMotionHistory = false;
    // When false, BLAS rebuild after deform does not force path-tracer accumulation reset
    // (geometry-sequence playback). Loop wraps still set ResetAccumulation via engine internals.
    bool resetAccumulationOnAccelRebuild = true;
};

// Mesh deformation / geometry-sequence playback via App. No WorldRenderer in the signature.
[[nodiscard]] std::vector<dm::float3> getMeshVertices(App& app, const std::shared_ptr<MeshInfo>& mesh);
[[nodiscard]] std::vector<dm::float3> getMeshVerticesWorld(App& app, const std::shared_ptr<MeshInfo>& mesh);
[[nodiscard]] std::vector<dm::float3> getMeshVerticesWorld(App& app, ecs::Entity entity);

void setMeshVertices(
    App& app,
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options = {});
void setMeshVerticesWorld(
    App& app,
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options = {});
void setMeshVerticesWorld(
    App& app,
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const SceneMeshEditOptions& options = {});

bool applyGeometrySequence(
    App& app,
    ecs::Entity entity,
    float timeSeconds,
    const SceneMeshEditOptions& options = {});

} // namespace caustica
