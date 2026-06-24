// AdvancedPathTracer is the concrete renderer wired up for the advanced sample
// (full path tracer + denoiser + RTXDI + ...). It is exposed via this header so
// the embedded Python interpreter and the Python extension module can both
// instantiate it in their respective hosts (caustica.exe vs caustica.pyd).

#pragma once

#include "caustica.h"
#include <render/Core/PTPipelineBaker.h>

class AdvancedPathTracer : public Sample
{
public:
    using Sample::Sample;

    void SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) override
    {
        if (m_ui.ActualUseRTXDIPasses())
            m_rtxdiPass->BeginFrame(commandList, *m_renderTargets, m_bindingLayout, m_bindingSet);

        PathTrace(framebuffer, constants);

        Denoise(framebuffer);
    }

    void CreateRTPipelines() override
    {
        auto pipelineBaker = GetRTPipelineBaker();
        using SM = caustica::ShaderMacro;

        // these don't actually compile any shaders - this happens later in m_ptPipelineBaker->Update
        m_ptPipelineReference         = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") }, "REF");
        m_ptPipelineBuildStablePlanes = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") }, "BUILD");
        m_ptPipelineFillStablePlanes  = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") }, "FILL");
        m_ptPipelineTestRaygenPPHDR   = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true);
        m_ptPipelineEdgeDetection     = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true);
    }

    void DestroyRTPipelines() override
    {
        m_ptPipelineReference         = nullptr;
        m_ptPipelineBuildStablePlanes = nullptr;
        m_ptPipelineFillStablePlanes  = nullptr;
        m_ptPipelineTestRaygenPPHDR   = nullptr;
        m_ptPipelineEdgeDetection     = nullptr;
    }

    std::string GetMaterialSpecializationShader() const override
    {
        return "PathTracerMaterialSpecializations.hlsl";
    }
};
