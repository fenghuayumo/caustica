#pragma once

#if CAUSTICA_WITH_DLSS

#include <memory>
#include <rhi/nvrhi.h>

class RenderTargets;

namespace caustica
{
    class ShaderFactory;
    class PlanarView;
}

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace caustica::render
{

    class DLSS
    {
    public:
        struct InitParameters
        {
            uint32_t inputWidth = 0;
            uint32_t inputHeight = 0;
            uint32_t outputWidth = 0;
            uint32_t outputHeight = 0;
            bool useLinearDepth = false;
            bool useAutoExposure = false;
            bool useRayReconstruction = false;
        };

        struct EvaluateParameters
        {
            nvrhi::TextureHandle depthTexture;
            nvrhi::TextureHandle motionVectorsTexture;
            nvrhi::TextureHandle inputColorTexture;
            nvrhi::TextureHandle outputColorTexture;

            // The exposure buffer returned by ToneMappingPass::getExposureBuffer(), optional.
            nvrhi::BufferHandle exposureBuffer;

            // DLSS RR (ray reconstruction) specific textures.
            nvrhi::TextureHandle diffuseAlbedo;
            nvrhi::TextureHandle specularAlbedo;
            nvrhi::TextureHandle normalRoughness;

            float exposureScale = 1.f;
            float sharpness = 0.f;
            float motionVectorScaleX = 1.f;
            float motionVectorScaleY = 1.f;
            bool resetHistory = false;
        };

        DLSS(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory);

        [[nodiscard]] bool isDlssSupported() const;
        [[nodiscard]] bool isDlssInitialized() const;

        [[nodiscard]] bool isRayReconstructionSupported() const;
        [[nodiscard]] bool isRayReconstructionInitialized() const;

        virtual void init(const InitParameters& params) = 0;

        virtual bool evaluate(
            nvrhi::ICommandList* commandList,
            const EvaluateParameters& params,
            const caustica::PlanarView& view) = 0;

        virtual ~DLSS() = default;

        static std::unique_ptr<DLSS> create(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID = DefaultApplicationID);

        static void getRequiredVulkanExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions);

        static const uint32_t DefaultApplicationID = 231313132;

    protected:
        bool m_dlssSupported = false;
        bool m_dlssInitialized = false;

        bool m_rayReconstructionSupported = false;
        bool m_rayReconstructionInitialized = false;

        NVSDK_NGX_Handle* m_dlssHandle = nullptr;
        NVSDK_NGX_Parameter* m_parameters = nullptr;

        InitParameters m_initParameters;

        nvrhi::DeviceHandle m_device;
        nvrhi::ShaderHandle m_exposureShader;
        nvrhi::ComputePipelineHandle m_exposurePipeline;
        nvrhi::TextureHandle m_exposureTexture;
        nvrhi::BufferHandle m_exposureSourceBuffer;
        nvrhi::BindingLayoutHandle m_exposureBindingLayout;
        nvrhi::BindingSetHandle m_exposureBindingSet;
        nvrhi::CommandListHandle m_featureCommandList;

        void computeExposure(nvrhi::ICommandList* commandList, nvrhi::IBuffer* toneMapperExposureBuffer, float exposureScale);
        
    #if CAUSTICA_WITH_DX11
        static std::unique_ptr<DLSS> createDX11(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    #if CAUSTICA_WITH_DX12
        static std::unique_ptr<DLSS> createDX12(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    #if CAUSTICA_WITH_VULKAN
        static std::unique_ptr<DLSS> createVK(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    };
}

#endif
