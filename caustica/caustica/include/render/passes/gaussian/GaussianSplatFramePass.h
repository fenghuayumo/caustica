#pragma once

#include <math/math.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <vector>

class AccumulationPass;
class RenderTargets;
class ShaderDebug;
struct PathTracerSettings;

namespace caustica
{
class AccelStructManager;
class ShaderFactory;
}

namespace caustica::render
{

class GPUSort;
class PathTracingContext;
class SceneGaussianSplatPasses;
struct FrameGraphContext;

// Per-frame Gaussian splat graph execute surface.
class GaussianSplatFramePass
{
public:
    GaussianSplatFramePass();
    ~GaussianSplatFramePass();

    GaussianSplatFramePass(const GaussianSplatFramePass&) = delete;
    GaussianSplatFramePass& operator=(const GaussianSplatFramePass&) = delete;

    void createTemporalResources(
        nvrhi::IDevice* device,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        RenderTargets* renderTargets);

    // Bind stable context + scene pass pointers once at create/prepare.
    void bindStable(
        PathTracingContext* context,
        nvrhi::IDevice* device,
        caustica::AccelStructManager* accelStructs,
        SceneGaussianSplatPasses* scenePasses);

    // Sync per-frame indices / sizes from the graph context.
    void bindFrame(const FrameGraphContext& ctx);

    void prepareScenePasses(const std::shared_ptr<ShaderDebug>& shaderDebug);

    void buildEmissionProxies(
        std::vector<GaussianSplatEmissionProxy>& outProxies,
        const PathTracerSettings& settings) const;

    [[nodiscard]] bool hasActiveSplats() const;
    [[nodiscard]] std::vector<GaussianSplatGraphResources> prepareGraphResources(bool renderToOutputColor);

    void executeAccelBuild(nvrhi::ICommandList* commandList);
    void executeUpload(nvrhi::ICommandList* commandList, bool renderToOutputColor);
    void executeSort(nvrhi::ICommandList* commandList);
    void executeRaster(nvrhi::ICommandList* commandList, bool renderToOutputColor);
    void executeAccumulate(nvrhi::ICommandList* commandList);

    [[nodiscard]] nvrhi::ITexture* currentColor() const { return m_currentColor.Get(); }
    [[nodiscard]] nvrhi::ITexture* accumulatedColor() const { return m_accumulatedColor.Get(); }

    [[nodiscard]] std::shared_ptr<GPUSort>& gpuSort() { return m_gpuSort; }
    [[nodiscard]] const std::shared_ptr<GPUSort>& gpuSort() const { return m_gpuSort; }

private:
    PathTracingContext* m_context = nullptr;
    nvrhi::IDevice* m_device = nullptr;
    RenderTargets* m_renderTargets = nullptr;
    caustica::AccelStructManager* m_accelStructs = nullptr;
    SceneGaussianSplatPasses* m_scenePasses = nullptr;
    dm::uint2 m_displaySize{};
    uint64_t m_frameIndex = 0;
    uint32_t m_sampleIndex = 0;
    int* m_temporalSampleIndex = nullptr;
    bool* m_frameTemporalReset = nullptr;
    bool* m_temporalReset = nullptr;

    nvrhi::TextureHandle m_currentColor;
    nvrhi::TextureHandle m_accumulatedColor;
    std::unique_ptr<AccumulationPass> m_accumulationPass;
    std::shared_ptr<GPUSort> m_gpuSort;
    bool m_compositeRendered = false;
};

} // namespace caustica::render
