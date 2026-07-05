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
class Scene;

struct UpdateSceneGeometryParams
{
    PathTracerSettings&              settings;
    bool&                            accelStructRebuildRequested;

    const std::shared_ptr<Scene>&    scene;
    nvrhi::ICommandList*             commandList = nullptr;
    MaterialGpuCache*                materials = nullptr;
    OpacityMicromapBuilder*          opacityMaps = nullptr;
    uint64_t                         frameIndex = 0;

    // OR-ed when OMM async builds are still in flight.
    bool*                            asyncLoadingInProgress = nullptr;
};

void updateSceneGeometry(AccelStructManager& accelStructs, UpdateSceneGeometryParams& params);

} // namespace caustica
