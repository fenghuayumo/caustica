#pragma once


#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <render/Core/RenderDevice.h>

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
    enum class TemporalAntiAliasingJitter
    {
        MSAA,
        Halton,
        R2,
        WhiteNoise
    };

    struct TemporalAntiAliasingParameters
    {
        float newFrameWeight = 0.1f;
        float clampingFactor = 1.0f;
        float maxRadiance = 10000.f;
        bool enableHistoryClamping = true;

        // Requires CreateParameters::historyClampRelax single channel [0, 1] mask to be provided. 
        // For texels with mask value of 0 the behavior is unchanged; for texels with mask value > 0, 
        // 'newFrameWeight' will be reduced and 'clampingFactor' will be increased proportionally. 
        bool useHistoryClampRelax = false;
    };

    class TemporalAntiAliasingPass
    {
    private:
        caustica::render::RenderDevice& m_renderDevice;

        nvrhi::ShaderHandle m_MotionVectorPS;
        nvrhi::ShaderHandle m_TemporalAntiAliasingCS;
        nvrhi::SamplerHandle m_BilinearSampler;
        nvrhi::BufferHandle m_TemporalAntiAliasingCB;

        nvrhi::BindingLayoutHandle m_MotionVectorsBindingLayout;
        nvrhi::BindingSetHandle m_MotionVectorsBindingSet;
        nvrhi::GraphicsPipelineHandle m_MotionVectorsPso;
        std::unique_ptr<caustica::FramebufferFactory> m_MotionVectorsFramebufferFactory;

        nvrhi::BindingLayoutHandle m_ResolveBindingLayout;
        nvrhi::BindingSetHandle m_ResolveBindingSet;
        nvrhi::BindingSetHandle m_ResolveBindingSetPrevious;
        nvrhi::ComputePipelineHandle m_ResolvePso;

        uint32_t m_FrameIndex;
        uint32_t m_StencilMask;
        dm::float2 m_ResolvedColorSize;

        dm::float2 m_R2Jitter;
        TemporalAntiAliasingJitter m_Jitter;

        bool m_HasHistoryClampRelaxTexture;

    public:
        struct CreateParameters
        {
            nvrhi::ITexture* sourceDepth = nullptr;
            nvrhi::ITexture* motionVectors = nullptr;
            nvrhi::ITexture* unresolvedColor = nullptr;
            nvrhi::ITexture* resolvedColor = nullptr;
            nvrhi::ITexture* feedback1 = nullptr;
            nvrhi::ITexture* feedback2 = nullptr;
            nvrhi::ITexture* historyClampRelax = nullptr;
            bool useCatmullRomFilter = true;
            uint32_t motionVectorStencilMask = 0;
            uint32_t numConstantBufferVersions = 16;
        };

        TemporalAntiAliasingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            const caustica::ICompositeView& compositeView,
            const CreateParameters& params);

        void RenderMotionVectors(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView,
            const caustica::ICompositeView& compositeViewPrevious,
            dm::float3 preViewTranslationDifference = dm::float3::zero());

        void TemporalResolve(
            nvrhi::ICommandList* commandList,
            const TemporalAntiAliasingParameters& params,
            bool feedbackIsValid,
            const caustica::ICompositeView& compositeViewInput,
            const caustica::ICompositeView& compositeViewOutput);

        void AdvanceFrame();
        void SetJitter(TemporalAntiAliasingJitter jitter);
        dm::float2 GetCurrentPixelOffset();
    };
}
