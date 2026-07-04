#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <unordered_map>
#include <render/Core/BindingCache.h>

namespace caustica::rhi
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
    class IView;
    class Scene;
    struct LightProbe;
}

namespace caustica::render
{
    class GBufferRenderTargets;
    
    class DeferredLightingPass
    {
    private:
        nvrhi::DeviceHandle m_Device;

        nvrhi::ShaderHandle m_ComputeShader;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::SamplerHandle m_ShadowSamplerComparison;
        nvrhi::BufferHandle m_DeferredLightingCB;
        nvrhi::ComputePipelineHandle m_Pso;

        nvrhi::BindingLayoutHandle m_BindingLayout;
        caustica::BindingCache m_BindingSets;

        rhi::RenderDevice* m_renderDevice = nullptr;

    protected:

        virtual nvrhi::ShaderHandle CreateComputeShader(
            caustica::ShaderFactory& shaderFactory);

    public:
        struct Inputs
        {
            nvrhi::ITexture* depth = nullptr;
            nvrhi::ITexture* gbufferNormals = nullptr;
            nvrhi::ITexture* gbufferDiffuse = nullptr;
            nvrhi::ITexture* gbufferSpecular = nullptr;
            nvrhi::ITexture* gbufferEmissive = nullptr;
            nvrhi::ITexture* indirectDiffuse = nullptr;
            nvrhi::ITexture* indirectSpecular = nullptr;
            nvrhi::ITexture* shadowChannels = nullptr;
            nvrhi::ITexture* ambientOcclusion = nullptr;
            nvrhi::ITexture* output = nullptr;

        const caustica::Scene* scene = nullptr;
        const std::vector<std::shared_ptr<caustica::LightProbe>>* lightProbes = nullptr;

            dm::float3 ambientColorTop = 0.f;
            dm::float3 ambientColorBottom = 0.f;

            // Fills the GBuffer-related textures (depth, normals, etc.) from the provided structure.
            void SetGBuffer(const GBufferRenderTargets& targets);
        };

        DeferredLightingPass(
            nvrhi::IDevice* device,
            rhi::RenderDevice& renderDevice);

        virtual void Init(const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

        void Render(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView,
            const Inputs& inputs,
            dm::float2 randomOffset = dm::float2::zero());

        void ResetBindingCache();
    };
}
