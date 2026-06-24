#pragma once

#define WITH_NRD 1

#if WITH_NRD

#include <NRD.h>
#include <rhi/nvrhi.h>
#include <unordered_map>
#include <render/Core/BindingCache.h>

class RenderTargets;

namespace caustica
{
    class PlanarView;
    class ShaderFactory;
}

class NrdIntegration
{
public:
    NrdIntegration(nvrhi::IDevice* device, nrd::Denoiser method);
    ~NrdIntegration();

    bool Initialize(uint32_t width, uint32_t height, caustica::ShaderFactory& shaderFactory);
    bool IsAvailable() const;

    void RunDenoiserPasses(
        nvrhi::ICommandList* commandList,
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

    const nrd::Denoiser GetDenoiser() const { return m_denoiser; }

private:
    nvrhi::DeviceHandle m_device;
    bool m_initialized;
    nrd::Instance* m_instance;
    nrd::Denoiser m_denoiser;
    nrd::Identifier m_identifier;

    struct NrdPipeline
    {
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle ResourcesBindingLayout;
        nvrhi::BindingLayoutHandle ConstantsAndSamplersBindingLayout;
        nvrhi::ComputePipelineHandle Pipeline;
    };

    nvrhi::BindingLayoutHandle CreateConstantsAndSamplersBindingLayout(const nrd::InstanceDesc& instanceDesc);
    nvrhi::BindingLayoutHandle CreateResourcesBindingLayout(const nrd::InstanceDesc& instanceDesc, const nrd::PipelineDesc& nrdPipelineDesc);

    nvrhi::BufferHandle m_constantBuffer;
    std::vector<NrdPipeline> m_pipelines;
    std::vector<nvrhi::SamplerHandle> m_samplers;
    std::vector<nvrhi::TextureHandle> m_permanentTextures;
    std::vector<nvrhi::TextureHandle> m_transientTextures;
    caustica::BindingCache m_bindingCache;
};

#endif