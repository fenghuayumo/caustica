#pragma once

#include <cstdint>

namespace nvrhi
{
class ICommandList;
}

namespace caustica
{
class Scene;
}

namespace caustica::render
{

class SceneGpuUpdater
{
public:
    static void refresh(Scene& scene, nvrhi::ICommandList* commandList, uint32_t frameIndex);

    // Upload mesh GPU buffers, then publish render proxies and fill instance buffers.
    // Prefer this over publishing a snapshot before EnsureMeshGpuBuffers.
    static void refreshAfterLoad(Scene& scene, uint32_t frameIndex);
};

} // namespace caustica::render
