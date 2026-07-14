#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica::render
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class ShadowMap;
    class FramebufferFactory;
    class ICompositeView;
}


namespace caustica::render
{
    struct SsaoParameters
    {
        float amount = 2.f;
        float backgroundViewDepth = 100.f;
        float radiusWorld = 0.5f;
        float surfaceBias = 0.1f;
        float powerExponent = 2.f;
        bool enableBlur = true;
        float blurSharpness = 16.f;
    };

    class SsaoPass
    {
    private:
        struct SubPass
        {
            nvrhi::ShaderHandle Shader;
            nvrhi::BindingLayoutHandle BindingLayout;
            std::vector<nvrhi::BindingSetHandle> BindingSets;
            nvrhi::ComputePipelineHandle Pipeline;
        };

        SubPass m_Deinterleave;
        SubPass m_Compute;
        SubPass m_Blur;

        nvrhi::DeviceHandle m_device;
        nvrhi::BufferHandle m_ConstantBuffer;

        nvrhi::TextureHandle m_DeinterleavedDepth;
        nvrhi::TextureHandle m_DeinterleavedOcclusion;
        dm::float2 m_QuantizedGbufferTextureSize;
                
        caustica::render::RenderDevice* m_renderDevice = nullptr;

    public:
        struct CreateParameters
        {
            dm::int2 dimensions = 0;
            bool inputLinearDepth = false;
            bool octEncodedNormals = false;
            bool directionalOcclusion = false;
            int numBindingSets = 1;
        };

        SsaoPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            const CreateParameters& params);

        SsaoPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            nvrhi::ITexture* gbufferDepth,
            nvrhi::ITexture* gbufferNormals,
            nvrhi::ITexture* destinationTexture);

        void createBindingSet(
            nvrhi::ITexture* gbufferDepth,
            nvrhi::ITexture* gbufferNormals,
            nvrhi::ITexture* destinationTexture,
            int bindingSetIndex = 0);

        void render(
            nvrhi::ICommandList* commandList,
            const SsaoParameters& params,
            const caustica::ICompositeView& compositeView,
            int bindingSetIndex = 0);
    };
}