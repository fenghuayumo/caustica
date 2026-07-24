#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

#include "GaussianSplatAccelBuilder.h"
#include "GaussianSplatEmissionProxy.h"
#include "GaussianSplatSorter.h"

#include <scene/GaussianSplatData.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <shaders/FrameConstantBuffer.h>

namespace caustica
{
    class FramebufferFactory;
    class IView;
    class ShaderFactory;
}

class GPUSort;
class RenderTargets;

enum class GaussianSplatFrustumCulling : uint32_t
{
    Disabled = 0,
    AtDistanceStage = 1,
    AtRasterStage = 2
};

enum class GaussianSplatStorageFormat : uint32_t
{
    Float32 = 0,
    Float16 = 1,
    Uint8 = 2
};

enum class GaussianSplatProjectionMethod : uint32_t
{
    Eigen = 0,
    Conic = 1
};

enum class GaussianSplatRenderTarget : uint32_t
{
    ProcessedOutputColor = 0,
    OutputColor = 1
};

struct GaussianSplatRenderSettings
{
    bool enabled = true;
    bool depthTest = true;
    bool shadowsEnabled = false;
    GaussianSplatSortMode sortingMode = GaussianSplatSortMode::GpuSort;
    GaussianSplatRenderTarget renderTarget = GaussianSplatRenderTarget::ProcessedOutputColor;
    GaussianSplatFrustumCulling frustumCulling = GaussianSplatFrustumCulling::AtRasterStage;
    GaussianSplatProjectionMethod projectionMethod = GaussianSplatProjectionMethod::Eigen;
    GaussianSplatStorageFormat shFormat = GaussianSplatStorageFormat::Uint8;
    GaussianSplatStorageFormat rgbaFormat = GaussianSplatStorageFormat::Uint8;
    bool screenSizeCulling = false;
    bool mipSplattingAntialiasing = false;
    bool useAABBs = false;
    bool useTLASInstances = true;
    bool blasCompaction = true;
    float splatScale = 1.0f;
    float alphaScale = 1.0f;
    float brightness = 1.0f;
    caustica::math::float3 tintColor = caustica::math::float3(1.0f);
    float alphaCullThreshold = 1.0f / 255.0f;
    float shadowStrength = 0.75f;
    float shadowRayTMax = 100000.0f;
    float shadowRayOffset = 0.20f;
    uint32_t shadowMode = GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    float shadowSoftRadius = 0.08f;
    uint32_t shadowSoftSampleCount = 1;
    uint32_t shadowFrameIndex = 0;
    float frustumDilation = 0.20f;
    float minPixelCoverage = 1.0f;
    uint32_t stochasticFrameIndex = 0;
    caustica::math::float3 shadowDirectionToLight = caustica::math::float3(0.0f, 1.0f, 0.0f);
    caustica::math::float4x4 objectToWorld = caustica::math::float4x4::identity();
};

struct GaussianSplatGraphResources
{
    caustica::rhi::Buffer* constantBuffer = nullptr;
    caustica::rhi::Buffer* splatBuffer = nullptr;
    caustica::rhi::Buffer* colorBuffer = nullptr;
    caustica::rhi::Buffer* shBuffer = nullptr;
    caustica::rhi::Buffer* indexBuffer = nullptr;
    caustica::rhi::Buffer* sortKeyBuffer = nullptr;
    caustica::rhi::Buffer* sortControlBuffer = nullptr;
    caustica::rhi::Buffer* drawIndirectBuffer = nullptr;
    caustica::rhi::Buffer* splatAabbBuffer = nullptr;
    caustica::rhi::Texture* stochasticDepth = nullptr;
    GaussianSplatSortMode sortMode = GaussianSplatSortMode::GpuSort;
    bool distanceStageCulling = false;
};

class GaussianSplatPass
{
public:
    GaussianSplatPass(
        caustica::rhi::Device* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);

    void setGpuSort(std::shared_ptr<GPUSort> gpuSort);

    bool loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub);

    void createPipeline(const RenderTargets& renderTargets);
    void buildAccelerationStructures(
        caustica::rhi::CommandList* commandList,
        bool useAABBs,
        bool useTLASInstances,
        bool allowBlasCompaction,
        float splatScale,
        uint32_t kernelDegree,
        bool adaptiveClamp);
    void releaseAccelerationStructures();
    void buildEmissionProxies(
        uint32_t maxProxyCount,
        float splatScale,
        uint32_t kernelDegree,
        bool adaptiveClamp,
        caustica::math::float3 tintColor,
        float alphaCullThreshold);

    void prepareGraphResources(const GaussianSplatRenderSettings& settings);
    [[nodiscard]] GaussianSplatGraphResources graphResources(const GaussianSplatRenderSettings& settings) const;

    bool upload(
        caustica::rhi::CommandList* commandList,
        const caustica::IView& view,
        caustica::rhi::rt::AccelStruct* meshTopLevelAS,
        const RenderTargets& renderTargets,
        const GaussianSplatRenderSettings& settings);
    void sort(caustica::rhi::CommandList* commandList);
    bool raster(
        caustica::rhi::CommandList* commandList,
        const caustica::IView& view);

    [[nodiscard]] bool hasSplats() const { return m_splatCount > 0; }
    [[nodiscard]] uint32_t getSplatCount() const { return m_splatCount; }
    [[nodiscard]] caustica::math::box3 getLocalBounds() const { return m_localBounds; }
    [[nodiscard]] const std::string& getSourceFileName() const { return m_sourceFileName; }
    [[nodiscard]] caustica::rhi::rt::AccelStruct* getTopLevelAS() const { return m_accelBuilder.getTopLevelAS(); }
    [[nodiscard]] caustica::rhi::Buffer* getSplatBuffer() const { return m_splatBuffer.Get(); }
    [[nodiscard]] uint32_t getShadowPrimitiveCountPerSplat() const { return m_accelBuilder.getShadowPrimitiveCountPerSplat(); }
    [[nodiscard]] bool getShadowUsesTLASInstances() const { return m_accelBuilder.getShadowUsesTLASInstances(); }
    [[nodiscard]] const std::vector<GaussianSplatEmissionProxy>& getEmissionProxies() const { return m_emissionProxies; }

private:
    void createBindingSets(const RenderTargets& renderTargets, caustica::rhi::rt::AccelStruct* meshTopLevelAS);
    void createStochasticFramebuffer(const RenderTargets& renderTargets);
    void ensureFormatBuffers(GaussianSplatStorageFormat shFormat, GaussianSplatStorageFormat rgbaFormat);
    void uploadSplatDataIfNeeded(caustica::rhi::CommandList* commandList);
    void uploadFormatDataIfNeeded(caustica::rhi::CommandList* commandList, GaussianSplatStorageFormat shFormat, GaussianSplatStorageFormat rgbaFormat);
    [[nodiscard]] caustica::render::GaussianSplatSortResources makeSortResources() const;

    caustica::rhi::DeviceHandle m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<GPUSort> m_gpuSort;

    caustica::rhi::BufferHandle m_constantBuffer;
    caustica::rhi::BufferHandle m_splatBuffer;
    caustica::rhi::BufferHandle m_colorBuffer;
    caustica::rhi::BufferHandle m_shBuffer;
    caustica::rhi::BufferHandle m_indexBuffer;
    caustica::rhi::BufferHandle m_sortKeyBuffer;
    caustica::rhi::BufferHandle m_sortControlBuffer;
    caustica::rhi::BufferHandle m_drawIndirectBuffer;
    caustica::rhi::BufferHandle m_splatAabbBuffer;
    caustica::rhi::TextureHandle m_stochasticDepthBuffer;
    caustica::rhi::TextureHandle m_stochasticProcessedDepthBuffer;

    caustica::rhi::BindingLayoutHandle m_rasterRenderBindingLayout;
    caustica::rhi::BindingLayoutHandle m_hybridRenderBindingLayout;
    caustica::rhi::BindingLayoutHandle m_sortKeyBindingLayout;
    caustica::rhi::BindingSetHandle m_rasterRenderBindingSet;
    caustica::rhi::BindingSetHandle m_hybridRenderBindingSet;
    caustica::rhi::BindingSetHandle m_sortKeyBindingSet;

    caustica::rhi::ShaderHandle m_rasterVertexShader;
    caustica::rhi::ShaderHandle m_rasterPixelShader;
    caustica::rhi::ShaderHandle m_hybridVertexShader;
    caustica::rhi::ShaderHandle m_hybridPixelShader;
    caustica::rhi::ShaderHandle m_sortKeyShader;
    caustica::rhi::GraphicsPipelineHandle m_rasterRenderPipeline;
    caustica::rhi::GraphicsPipelineHandle m_hybridRenderPipeline;
    caustica::rhi::GraphicsPipelineHandle m_stochasticRasterRenderPipeline;
    caustica::rhi::GraphicsPipelineHandle m_stochasticHybridRenderPipeline;
    caustica::rhi::GraphicsPipelineHandle m_stochasticProcessedRasterRenderPipeline;
    caustica::rhi::GraphicsPipelineHandle m_stochasticProcessedHybridRenderPipeline;
    caustica::rhi::ComputePipelineHandle m_sortKeyPipeline;
    caustica::render::GaussianSplatAccelBuilder m_accelBuilder;
    caustica::render::GaussianSplatSorter m_sorter;
    caustica::rhi::rt::AccelStruct* m_hybridRenderMeshTopLevelAS = nullptr;
    std::shared_ptr<caustica::FramebufferFactory> m_stochasticFramebuffer;
    std::shared_ptr<caustica::FramebufferFactory> m_stochasticProcessedFramebuffer;

    std::vector<caustica::GaussianSplatData> m_splats;
    std::vector<caustica::math::float4> m_colorOpacity;
    std::vector<caustica::math::float4> m_shCoefficients;
    std::vector<GaussianSplatEmissionProxy> m_emissionProxies;
    std::vector<uint8_t> m_packedColorOpacity;
    std::vector<uint8_t> m_packedShCoefficients;
    caustica::math::box3 m_localBounds = caustica::math::box3::empty();
    bool m_localBoundsValid = false;
    uint32_t m_splatCount = 0;
    uint32_t m_shDegree = 0;
    bool m_splatUploadPending = false;
    bool m_formatUploadPending = true;
    uint32_t m_cachedEmissionProxyMaxCount = 0;
    float m_cachedEmissionProxySplatScale = 1.0f;
    uint32_t m_cachedEmissionProxyKernelDegree = 0;
    bool m_cachedEmissionProxyAdaptiveClamp = true;
    caustica::math::float3 m_cachedEmissionProxyTintColor = caustica::math::float3(1.0f);
    float m_cachedEmissionProxyAlphaCullThreshold = 0.0f;
    bool m_emissionProxyBuildPending = true;
    GaussianSplatStorageFormat m_currentShFormat = GaussianSplatStorageFormat::Float32;
    GaussianSplatStorageFormat m_currentRgbaFormat = GaussianSplatStorageFormat::Float32;
    GaussianSplatRenderSettings m_frameRenderSettings;
    GaussianSplatConstants m_frameConstants = {};
    caustica::rhi::BindingSetHandle m_frameRenderBindingSet;
    caustica::rhi::GraphicsPipelineHandle m_frameRenderPipeline;
    caustica::rhi::Framebuffer* m_frameFramebuffer = nullptr;
    caustica::rhi::TextureHandle m_frameStochasticDepthBuffer;
    bool m_framePrepared = false;
    bool m_frameDistanceStageCulling = false;
    std::string m_sourceFileName;
};
