#pragma once

#include <scene/SceneObjects.h>
#include <render/passes/lighting/MaterialGpuCache.h> // for MaterialEx

namespace caustica::render
{

// =============================================================================
// RenderSceneTypeFactory — SceneTypeFactory that creates render-specific types.
//
// Only overrides createMaterial() to produce MaterialEx (StandardMaterial).
// Mesh / camera instances are ECS components; factory only creates mesh/material assets.
// =============================================================================
class RenderSceneTypeFactory : public SceneTypeFactory
{
public:
    std::shared_ptr<Material> createMaterial() override
    {
        return std::static_pointer_cast<Material>(std::make_shared<MaterialEx>());
    }
};

} // namespace caustica::render
