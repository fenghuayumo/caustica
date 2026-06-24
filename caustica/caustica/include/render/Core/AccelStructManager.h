#pragma once

#include <rhi/nvrhi.h>
#include <math/math.h>
#include <shaders/SubInstanceData.h>
#include <scene/SceneTypes.h>

#include <cstdint>
#include <memory>
#include <vector>

class OmmBaker;

namespace caustica
{
class Scene;

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
// Extracted from editor PathTracerApp (Phase C).
// =============================================================================
class AccelStructManager
{
public:
    AccelStructManager() = default;
    explicit AccelStructManager(nvrhi::IDevice* device);

    void createBlases(nvrhi::ICommandList* commandList,
                      const Scene&         scene,
                      const AccelStructBuildSettings& settings);

    void createTlas(nvrhi::ICommandList* commandList, const Scene& scene);

    void uploadSubInstanceData(nvrhi::ICommandList* commandList) const;

    void clearMeshAccelStructs(Scene& scene);

    void requestMeshRebuild(const std::shared_ptr<MeshInfo>& mesh);

    void rebuildDirtyMeshes(nvrhi::ICommandList*            commandList,
                            const Scene&                    scene,
                            const AccelStructBuildSettings& settings,
                            bool&                           fullRebuildRequested);

    void updateSkinnedBlases(nvrhi::ICommandList*            commandList,
                             const Scene&                    scene,
                             const AccelStructBuildSettings& settings,
                             uint32_t                        frameIndex) const;

    void buildTlas(nvrhi::ICommandList*            commandList,
                   const Scene&                    scene,
                   const AccelStructBuildSettings& settings,
                   const OmmAccelStructState&      ommState,
                   ::OmmBaker*                     ommBaker) const;

    void releaseGpuResources();

    [[nodiscard]] nvrhi::rt::AccelStructHandle getTopLevelAS() const { return m_topLevelAS; }
    [[nodiscard]] nvrhi::BufferHandle          getSubInstanceBuffer() const { return m_subInstanceBuffer; }
    [[nodiscard]] std::vector<SubInstanceData>& getSubInstanceData() { return m_subInstanceData; }
    [[nodiscard]] const std::vector<SubInstanceData>& getSubInstanceData() const { return m_subInstanceData; }
    [[nodiscard]] uint32_t                       getSubInstanceCount() const { return m_subInstanceCount; }
    void                                         resetSubInstanceCount() { m_subInstanceCount = 0; }
    [[nodiscard]] bool                           hasTopLevelAS() const { return m_topLevelAS != nullptr; }

private:
    nvrhi::IDevice* m_device = nullptr;

    nvrhi::rt::AccelStructHandle                 m_topLevelAS;
    nvrhi::BufferHandle                          m_subInstanceBuffer;
    std::vector<SubInstanceData>                 m_subInstanceData;
    uint32_t                                         m_subInstanceCount = 0;
    std::vector<std::shared_ptr<MeshInfo>>       m_meshesPendingAccelRebuild;
};

} // namespace caustica
