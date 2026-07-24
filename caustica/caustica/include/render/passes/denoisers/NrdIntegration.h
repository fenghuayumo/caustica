#pragma once

#define WITH_NRD 1

#if WITH_NRD

#include <NRD.h>
#include <rhi/rhi.h>
#include <unordered_map>
#include <render/core/BindingCache.h>

class RenderTargets;

namespace caustica
{
    class PlanarView;
    class ShaderFactory;
}

class NrdIntegration
{
public:
    NrdIntegration(caustica::rhi::IDevice* device, nrd::Denoiser method);
    ~NrdIntegration();

    bool initialize(uint32_t width, uint32_t height, caustica::ShaderFactory& shaderFactory);
    bool isAvailable() const;

    void runDenoiserPasses(
        caustica::rhi::ICommandList* commandList,
        const RenderTargets& renderTargets,
        int pass,
        const caustica::PlanarView& view, 
        const caustica::PlanarView& viewPrev,
        uint32_t frameIndex,
        float disocclusionThreshold,
        float disocclusionThresholdAlternate,
        bool useDisocclusionThresholdAlternateMix,
        float timeDeltaBetweenFrames, // < 0 to track internally in NRD
        bool enableValidation,
        bool resetHistory,
        const void* methodSettings);

    const nrd::Denoiser getDenoiser() const { return m_denoiser; }

private:
    caustica::rhi::DeviceHandle m_device;
    bool m_initialized;
    nrd::Instance* m_instance;
    nrd::Denoiser m_denoiser;
    nrd::Identifier m_identifier;

    struct NrdPipeline
    {
        caustica::rhi::ShaderHandle Shader;
        caustica::rhi::BindingLayoutHandle ResourcesBindingLayout;
        caustica::rhi::BindingLayoutHandle ConstantsAndSamplersBindingLayout;
        caustica::rhi::ComputePipelineHandle Pipeline;
    };

    caustica::rhi::BindingLayoutHandle createConstantsAndSamplersBindingLayout(const nrd::InstanceDesc& instanceDesc);
    caustica::rhi::BindingLayoutHandle createResourcesBindingLayout(const nrd::InstanceDesc& instanceDesc, const nrd::PipelineDesc& nrdPipelineDesc);

    caustica::rhi::BufferHandle m_constantBuffer;
    std::vector<NrdPipeline> m_pipelines;
    std::vector<caustica::rhi::SamplerHandle> m_samplers;
    std::vector<caustica::rhi::TextureHandle> m_permanentTextures;
    std::vector<caustica::rhi::TextureHandle> m_transientTextures;
    caustica::BindingCache m_bindingCache;
};

#endif