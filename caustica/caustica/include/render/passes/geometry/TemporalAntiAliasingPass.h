#pragma once


#include <math/math.h>
#include <rhi/rhi.h>
#include <memory>
#include <render/core/RenderDevice.h>
#include <render/core/TemporalAntiAliasingParameters.h>

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ViewInfo;
}

namespace caustica::render
{
    class TemporalAntiAliasingPass
    {
    private:
        caustica::render::RenderDevice& m_renderDevice;

        caustica::rhi::ShaderHandle m_MotionVectorPS;
        caustica::rhi::ShaderHandle m_TemporalAntiAliasingCS;
        caustica::rhi::SamplerHandle m_BilinearSampler;
        caustica::rhi::BufferHandle m_TemporalAntiAliasingCB;

        caustica::rhi::BindingLayoutHandle m_MotionVectorsBindingLayout;
        caustica::rhi::BindingSetHandle m_MotionVectorsBindingSet;
        caustica::rhi::GraphicsPipelineHandle m_MotionVectorsPso;
        std::unique_ptr<caustica::FramebufferFactory> m_MotionVectorsFramebufferFactory;

        caustica::rhi::BindingLayoutHandle m_ResolveBindingLayout;
        caustica::rhi::BindingSetHandle m_ResolveBindingSet;
        caustica::rhi::BindingSetHandle m_ResolveBindingSetPrevious;
        caustica::rhi::ComputePipelineHandle m_ResolvePso;

        uint32_t m_FrameIndex;
        uint32_t m_StencilMask;
        math::float2 m_ResolvedColorSize;

        math::float2 m_R2Jitter;
        TemporalAntiAliasingJitter m_Jitter;

        bool m_HasHistoryClampRelaxTexture;

    public:
        struct CreateParameters
        {
            caustica::rhi::Texture* sourceDepth = nullptr;
            caustica::rhi::Texture* motionVectors = nullptr;
            caustica::rhi::Texture* unresolvedColor = nullptr;
            caustica::rhi::Texture* resolvedColor = nullptr;
            caustica::rhi::Texture* feedback1 = nullptr;
            caustica::rhi::Texture* feedback2 = nullptr;
            caustica::rhi::Texture* historyClampRelax = nullptr;
            bool useCatmullRomFilter = true;
            uint32_t motionVectorStencilMask = 0;
            uint32_t numConstantBufferVersions = 16;
        };

        TemporalAntiAliasingPass(
            caustica::rhi::Device* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            const caustica::ViewInfo& compositeView,
            const CreateParameters& params);

        void renderMotionVectors(
            caustica::rhi::CommandList* commandList,
            const caustica::ViewInfo& compositeView,
            const caustica::ViewInfo& compositeViewPrevious,
            math::float3 preViewTranslationDifference = math::float3::zero());

        void temporalResolve(
            caustica::rhi::CommandList* commandList,
            const TemporalAntiAliasingParameters& params,
            bool feedbackIsValid,
            const caustica::ViewInfo& compositeViewInput,
            const caustica::ViewInfo& compositeViewOutput);

        void advanceFrame();
        void setJitter(TemporalAntiAliasingJitter jitter);
        math::float2 getCurrentPixelOffset();
    };
}
