#pragma once

#include <render/core/PathTracerSettings.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <memory>

class MaterialGpuCache;
class OpacityMicromapBuilder;

namespace caustica
{
class AccelStructManager;
class IDescriptorTableManager;
class Scene;

namespace scene
{
class SceneRenderData;
}

namespace render
{
struct SceneGpuResources;
}

struct UpdateSceneGeometryParams
{
    PathTracerSettings&              settings;
    bool&                            accelStructRebuildRequested;

    // Session scene for GPU upload / OMM / BLAS mutation. Prefer renderData for reads.
    const std::shared_ptr<Scene>&    scene;
    const scene::SceneRenderData*    renderData = nullptr;
    render::SceneGpuResources*       gpuResources = nullptr;

    nvrhi::ICommandList*             commandList = nullptr;
    IDescriptorTableManager*         descriptorTable = nullptr;
    MaterialGpuCache*                materials = nullptr;
    OpacityMicromapBuilder*          opacityMaps = nullptr;
    uint64_t                         frameIndex = 0;

    // OR-ed when OMM async builds are still in flight.
    bool*                            asyncLoadingInProgress = nullptr;
};

void updateSceneGeometry(AccelStructManager& accelStructs, UpdateSceneGeometryParams& params);

} // namespace caustica
