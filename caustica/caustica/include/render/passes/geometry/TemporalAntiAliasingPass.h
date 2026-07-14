#pragma once


#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <render/core/RenderDevice.h>
#include <render/core/TemporalAntiAliasingParameters.h>

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
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

        void renderMotionVectors(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView,
            const caustica::ICompositeView& compositeViewPrevious,
            dm::float3 preViewTranslationDifference = dm::float3::zero());

        void temporalResolve(
            nvrhi::ICommandList* commandList,
            const TemporalAntiAliasingParameters& params,
            bool feedbackIsValid,
            const caustica::ICompositeView& compositeViewInput,
            const caustica::ICompositeView& compositeViewOutput);

        void advanceFrame();
        void setJitter(TemporalAntiAliasingJitter jitter);
        dm::float2 getCurrentPixelOffset();
    };
}
