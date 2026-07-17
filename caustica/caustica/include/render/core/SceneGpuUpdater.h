#pragma once

#include <cstdint>

namespace nvrhi
{
class ICommandList;
}

namespace caustica
{
class IDescriptorTableManager;
class Scene;
namespace scene { class SceneRenderData; }
}

namespace caustica::render
{

class SceneGpuUpdater
{
public:
    static void refresh(
        Scene& scene,
        IDescriptorTableManager* descriptorTable,
        nvrhi::ICommandList* commandList,
        uint32_t frameIndex);

    // Exclusive GPU setup from logic-thread staging data. Does not publish snapshots.
    static void refreshAfterLoad(
        Scene& scene,
        const scene::SceneRenderData& renderData,
        IDescriptorTableManager* descriptorTable,
        uint32_t frameIndex);
};

} // namespace caustica::render
