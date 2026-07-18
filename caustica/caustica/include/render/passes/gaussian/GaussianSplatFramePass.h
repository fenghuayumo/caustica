#pragma once

#include <math/math.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <vector>

class AccumulationPass;
class RenderTargets;
class ShaderDebug;

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

// Per-frame Gaussian splat graph execute surface (textures + upload/sort/raster/accumulate).
class GaussianSplatFramePass
{
public:
    GaussianSplatFramePass();
    ~GaussianSplatFramePass();

    GaussianSplatFramePass(const GaussianSplatFramePass&) = delete;
    GaussianSplatFramePass& operator=(const GaussianSplatFramePass&) = delete;

    struct FrameBindings
    {
        PathTracingContext* context = nullptr;
        nvrhi::IDevice* device = nullptr;
        RenderTargets* renderTargets = nullptr;
        caustica::AccelStructManager* accelStructs = nullptr;
        SceneGaussianSplatPasses* scenePasses = nullptr;
        dm::uint2 displaySize{};
        uint64_t frameIndex = 0;
        uint32_t sampleIndex = 0;
        int* temporalSampleIndex = nullptr;
        bool* frameTemporalReset = nullptr;
        bool* temporalReset = nullptr;
    };

    void createTemporalResources(
        nvrhi::IDevice* device,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        RenderTargets* renderTargets);

    void bindFrame(const FrameBindings& bindings);

    void prepareScenePasses(const std::shared_ptr<ShaderDebug>& shaderDebug);

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
    FrameBindings m_bindings{};
    nvrhi::TextureHandle m_currentColor;
    nvrhi::TextureHandle m_accumulatedColor;
    std::unique_ptr<AccumulationPass> m_accumulationPass;
    std::shared_ptr<GPUSort> m_gpuSort;
    bool m_compositeRendered = false;
};

} // namespace caustica::render
