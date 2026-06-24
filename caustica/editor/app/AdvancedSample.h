// AdvancedPathTracer is the concrete renderer wired up for the advanced sample
// (full path tracer + denoiser + RTXDI + ...). It is exposed via this header so
// the embedded Python interpreter and the Python extension module can both
// instantiate it in their respective hosts (caustica.exe vs caustica.pyd).

#pragma once

#include "PathTracerApp.h"
#include "render/PathTracingRenderer.h"
#include <render/Core/PTPipelineBaker.h>

class AdvancedPathTracer : public PathTracerApp
{
public:
    using PathTracerApp::PathTracerApp;

    void SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) override
    {
        auto& r = *m_pathTracingRenderer;
        if (m_ui.ActualUseRTXDIPasses())
            r.getRtxdiPass()->BeginFrame(commandList, *r.getRenderTargets(), r.getBindingLayout(), r.getBindingSet());

        PathTrace(framebuffer, constants);

        Denoise(framebuffer);
    }

    void CreateRTPipelines() override
    {
        auto pipelineBaker = GetRTPipelineBaker();
        using SM = caustica::ShaderMacro;

        PtPipelineReference()         = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") }, "REF");
        PtPipelineBuildStablePlanes() = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") }, "BUILD");
        PtPipelineFillStablePlanes()  = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") }, "FILL");
        PtPipelineTestRaygenPPHDR()   = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true);
        PtPipelineEdgeDetection()     = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true);
    }

    void DestroyRTPipelines() override
    {
        PtPipelineReference()         = nullptr;
        PtPipelineBuildStablePlanes() = nullptr;
        PtPipelineFillStablePlanes()  = nullptr;
        PtPipelineTestRaygenPPHDR()   = nullptr;
        PtPipelineEdgeDetection()     = nullptr;
    }

    std::string GetMaterialSpecializationShader() const override
    {
        return "PathTracerMaterialSpecializations.hlsl";
    }
};
