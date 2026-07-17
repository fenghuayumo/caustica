#pragma once

#include <cstdint>

namespace nvrhi
{
class ICommandList;
class IDevice;
}

namespace caustica
{
class IDescriptorTableManager;
class Scene;
class ShaderFactory;
namespace scene { class SceneRenderData; }
}

namespace caustica::render
{

struct SceneGpuResources;

class SceneGpuUpdater
{
public:
    static void initialize(
        SceneGpuResources& gpuResources,
        nvrhi::IDevice* device,
        ShaderFactory& shaderFactory);

    static void refresh(
        Scene& scene,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        nvrhi::ICommandList* commandList,
        uint32_t frameIndex);

    // Exclusive GPU setup from logic-thread staging data. Does not publish snapshots.
    static void refreshAfterLoad(
        Scene& scene,
        const scene::SceneRenderData& renderData,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        uint32_t frameIndex);
};

} // namespace caustica::render
