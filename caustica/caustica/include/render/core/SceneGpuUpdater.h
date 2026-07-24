#pragma once

#include <cstdint>
#include <rhi/rhi.h>

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
        caustica::rhi::Device* device,
        ShaderFactory& shaderFactory);

    static void refresh(
        Scene& scene,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        caustica::rhi::CommandList* commandList,
        uint32_t frameIndex);

    // Exclusive GPU setup from logic-thread staging data. Does not publish snapshots.
    // pruneRemovedResources=false keeps GPU records referenced by a live/retired TLAS
    // during async double-buffered structure rebuild.
    static void refreshAfterLoad(
        Scene& scene,
        const scene::SceneRenderData& renderData,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        uint32_t frameIndex,
        bool pruneRemovedResources = true);
};

} // namespace caustica::render
