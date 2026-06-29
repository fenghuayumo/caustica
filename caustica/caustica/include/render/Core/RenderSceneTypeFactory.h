#pragma once

#include <scene/SceneGraph.h>
#include <render/Passes/Lighting/MaterialGpuCache.h> // for MaterialEx

namespace caustica::render
{

// =============================================================================
// RenderSceneTypeFactory — SceneTypeFactory that creates render-specific types.
//
// Only overrides CreateMaterial() to produce MaterialEx (which adds the
// path-tracing PTMaterial data). MeshInfo / MeshGeometry have been fully
// merged into the base types and no longer need overrides.
// =============================================================================
class RenderSceneTypeFactory : public SceneTypeFactory
{
public:
    std::shared_ptr<Material> CreateMaterial() override
    {
        return std::static_pointer_cast<Material>(std::make_shared<MaterialEx>());
    }
};

} // namespace caustica::render
