#pragma once

#include <rhi/rhi.h>
#include <math/math.h>
#include <shaders/SubInstanceData.h>
#include <scene/SceneRenderData.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

class OpacityMicromapBuilder;
class MaterialGpuCache;

namespace caustica
{
namespace scene
{
class SceneRenderData;
}
namespace render { struct SceneGpuResources; }

struct AccelStructBuildSettings
{
    bool excludeTransmissive = false;
    bool forceOpaque         = false;
};

struct OmmAccelStructState
{
    bool enabled           = false;
    bool force2State       = false;
    bool onlyOMMs          = false;
    bool debugViewEnabled  = false;
};

// =============================================================================
// AccelStructManager — BLAS / TLAS lifecycle and sub-instance GPU buffers.
// Extracted from editor SceneEditor (Phase C).
// =============================================================================
class AccelStructManager
{
public:
    AccelStructManager() = default;
    explicit AccelStructManager(caustica::rhi::Device* device);
    void bindSceneGpuResources(render::SceneGpuResources* resources) { m_sceneGpuResources = resources; }
    void bindMaterialGpuCache(MaterialGpuCache* materials) { m_materialGpuCache = materials; }

    void createBlases(caustica::rhi::CommandList* commandList,
                      std::span<const scene::MeshRenderResourceSnapshot> meshes,
                      const AccelStructBuildSettings& settings);

    void createTlas(caustica::rhi::CommandList* commandList, const scene::SceneRenderData& renderData);

    void uploadSubInstanceData(caustica::rhi::CommandList* commandList) const;

    void clearMeshAccelStructs(std::span<const scene::MeshRenderResourceSnapshot> meshes);

    void requestMeshRebuild(scene::MeshRenderResourceId meshId);

    void rebuildDirtyMeshes(caustica::rhi::CommandList*            commandList,
                            const scene::SceneRenderData&   renderData,
                            const AccelStructBuildSettings& settings,
                            bool&                           fullRebuildRequested);

    void updateSkinnedBlases(caustica::rhi::CommandList*            commandList,
                             const scene::SceneRenderData&   renderData,
                             const AccelStructBuildSettings& settings,
                             uint32_t                        frameIndex) const;

    void buildTlas(caustica::rhi::CommandList*            commandList,
                   const scene::SceneRenderData&   renderData,
                   const AccelStructBuildSettings& settings,
                   const OmmAccelStructState&      ommState,
                   ::OpacityMicromapBuilder*                     opacityMicromapBuilder) const;

    void releaseGpuResources();

    // Drop AS handles retired by the previous double-buffered rebuild. Safe once
    // no in-flight frame still references that generation (next structure edit).
    void clearRetiredAccelStructs();

    [[nodiscard]] caustica::rhi::rt::AccelStructHandle getTopLevelAS() const { return m_topLevelAS; }
    [[nodiscard]] caustica::rhi::BufferHandle          getSubInstanceBuffer() const { return m_subInstanceBuffer; }
    [[nodiscard]] std::vector<SubInstanceData>& getSubInstanceData() { return m_subInstanceData; }
    [[nodiscard]] const std::vector<SubInstanceData>& getSubInstanceData() const { return m_subInstanceData; }
    [[nodiscard]] uint32_t                       getSubInstanceCount() const { return m_subInstanceCount; }
    void                                         resetSubInstanceCount() { m_subInstanceCount = 0; }
    [[nodiscard]] bool                           hasTopLevelAS() const { return m_topLevelAS != nullptr; }

private:
    caustica::rhi::Device* m_device = nullptr;
    render::SceneGpuResources* m_sceneGpuResources = nullptr;
    MaterialGpuCache* m_materialGpuCache = nullptr;
    uint64_t m_materialStateRevision = 0;

    caustica::rhi::rt::AccelStructHandle                 m_topLevelAS;
    caustica::rhi::BufferHandle                          m_subInstanceBuffer;
    std::vector<SubInstanceData>                 m_subInstanceData;
    uint32_t                                         m_subInstanceCount = 0;
    // Previous generation kept alive so in-flight DispatchRays can finish against
    // the old TLAS/BLAS while the new generation is built and bound.
    std::vector<caustica::rhi::rt::AccelStructHandle> m_retiredTopLevelAS;
    std::vector<caustica::rhi::BufferHandle>          m_retiredSubInstanceBuffers;
    std::vector<caustica::rhi::rt::AccelStructHandle> m_retiredBlas;
    std::vector<scene::MeshRenderResourceId>     m_meshesPendingAccelRebuild;
    std::shared_ptr<std::mutex>                  m_pendingRebuildMutex =
        std::make_shared<std::mutex>();
};

} // namespace caustica
