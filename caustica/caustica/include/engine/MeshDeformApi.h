#pragma once

#include <ecs/Entity.h>
#include <math/math.h>

#include <cstdint>
#include <vector>

namespace caustica
{

class App;

// App-facing mesh deform options. GPU upload / AS rebuild stay inside the engine.
// (Low-level GPU wiring: engine/internal/MeshDeformGpu.h — apps must not include.)
struct MeshDeformOptions
{
    bool recomputeNormals = true;
    bool rebuildAccelerationStructure = true;
    // Write Position and PrevPosition so motion vectors are zero (loop wrap / first sample).
    bool zeroMotionHistory = false;
    // When false, BLAS rebuild after deform does not force path-tracer accumulation reset
    // (geometry-sequence playback). Loop wraps still set ResetAccumulation via engine internals.
    bool resetAccumulationOnAccelRebuild = true;
};

[[nodiscard]] std::vector<math::float3> getMeshVertices(App& app, ecs::Entity entity);
[[nodiscard]] std::vector<math::float3> getMeshVerticesWorld(App& app, ecs::Entity entity);

void setMeshVertices(
    App& app,
    ecs::Entity entity,
    const std::vector<math::float3>& vertices,
    const MeshDeformOptions& options = {});
void setMeshVerticesWorld(
    App& app,
    ecs::Entity entity,
    const std::vector<math::float3>& vertices,
    const MeshDeformOptions& options = {});

bool applyGeometrySequence(
    App& app,
    ecs::Entity entity,
    float timeSeconds,
    const MeshDeformOptions& options = {});

} // namespace caustica
