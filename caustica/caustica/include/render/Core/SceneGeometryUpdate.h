#pragma once

#include <render/Core/PathTracerSettings.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <memory>

class MaterialsBaker;
class OmmBaker;

namespace caustica
{
class Scene;

struct UpdateSceneGeometryParams
{
    PathTracerSettings&              settings;
    bool&                            accelStructRebuildRequested;

    const std::shared_ptr<Scene>&    scene;
    nvrhi::ICommandList*             commandList = nullptr;
    MaterialsBaker*                  materialsBaker = nullptr;
    OmmBaker*                        ommBaker = nullptr;
    uint64_t                         frameIndex = 0;

    // OR-ed when OMM async builds are still in flight.
    bool*                            asyncLoadingInProgress = nullptr;
};

} // namespace caustica
