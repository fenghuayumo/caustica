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

    // Spawn / sync helpers: upload all meshes then finalize (each mesh drains GPU).
    static void refreshAfterLoad(
        Scene& scene,
        const scene::SceneRenderData& renderData,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        uint32_t frameIndex,
        bool pruneRemovedResources = true);

    // Multi-frame bind building blocks (RT-only).
    static size_t uploadMeshesAfterLoad(
        const scene::SceneRenderData& renderData,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        size_t meshBegin,
        size_t maxMeshes);

    static void finalizeAfterLoad(
        Scene& scene,
        const scene::SceneRenderData& renderData,
        SceneGpuResources& gpuResources,
        IDescriptorTableManager* descriptorTable,
        uint32_t frameIndex,
        bool pruneRemovedResources = true);
};

} // namespace caustica::render
