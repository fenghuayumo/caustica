#pragma once

#include <rhi/nvrhi.h>
#include <scene/GaussianSplatData.h>

#include <cstdint>
#include <vector>

namespace caustica::render
{

struct GaussianSplatAccelBuildParams
{
    bool useAABBs = false;
    bool useTLASInstances = true;
    bool allowBlasCompaction = true;
    float splatScale = 1.0f;
    uint32_t kernelDegree = 0;
    bool adaptiveClamp = true;
};

class GaussianSplatAccelBuilder
{
public:
    explicit GaussianSplatAccelBuilder(nvrhi::IDevice* device);

    void invalidate();
    void release(bool markBuildPending = true);

    void build(
        nvrhi::ICommandList* commandList,
        const GaussianSplatAccelBuildParams& params,
        const std::vector<caustica::GaussianSplatData>& splats,
        uint32_t splatCount,
        nvrhi::IBuffer* aabbBuffer);

    [[nodiscard]] nvrhi::rt::IAccelStruct* getTopLevelAS() const { return m_topLevelAS.Get(); }
    [[nodiscard]] nvrhi::rt::IAccelStruct* getBottomLevelAS() const { return m_bottomLevelAS.Get(); }
    [[nodiscard]] uint32_t getShadowPrimitiveCountPerSplat() const { return m_shadowPrimitiveCountPerSplat; }
    [[nodiscard]] bool getShadowUsesTLASInstances() const { return m_lastUseTLASInstances; }
    [[nodiscard]] bool isBuildPending() const { return m_buildPending; }

private:
    nvrhi::DeviceHandle m_device;

    nvrhi::BufferHandle m_triangleVertexBuffer;
    nvrhi::BufferHandle m_triangleIndexBuffer;
    nvrhi::rt::AccelStructHandle m_bottomLevelAS;
    nvrhi::rt::AccelStructHandle m_topLevelAS;

    bool m_buildPending = false;
    bool m_lastBlasCompaction = false;
    bool m_lastUseAABBs = true;
    bool m_lastUseTLASInstances = false;
    float m_lastSplatScale = 1.0f;
    uint32_t m_lastKernelDegree = 2;
    bool m_lastAdaptiveClamp = true;
    uint32_t m_shadowPrimitiveCountPerSplat = 1;
};

} // namespace caustica::render
