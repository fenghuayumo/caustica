#!/usr/bin/env python3
"""Replace m_app.<render-member> with m_<render-member> in PathTracingRenderer.cpp."""

from pathlib import Path

RENDER_MEMBERS = [
    "m_rtxdiPass",
    "m_renderTargets",
    "m_bindingLayout",
    "m_bindlessLayout",
    "m_bindingSet",
    "m_ptPipelineReference",
    "m_ptPipelineBuildStablePlanes",
    "m_ptPipelineFillStablePlanes",
    "m_ptPipelineTestRaygenPPHDR",
    "m_ptPipelineEdgeDetection",
    "m_commandList",
    "m_temporalAntiAliasingPass",
    "m_bloomPass",
    "m_toneMappingPass",
    "m_constantBuffer",
    "m_postProcess",
    "m_nrd",
    "m_accumulationPass",
    "m_oidnDenoiser",
    "m_oidnDenoisedOutput",
    "m_oidnDenoisedOutputValid",
    "m_oidnDenoiserFailed",
    "m_shaderDebug",
    "m_denoisingGuidesBaker",
    "m_exportVBufferCS",
    "m_exportVBufferPSO",
    "m_nativeDLSS",
    "m_recommendedDLSSSettings",
    "m_lastDLSSRROptions",
    "m_renderSize",
    "m_displaySize",
    "m_displayAspectRatio",
    "m_frameIndex",
    "m_sampleIndex",
    "m_accumulationSampleIndex",
    "m_currentConstants",
    "m_accumulationCompleted",
    "m_gaussianSplatCurrentColor",
    "m_gaussianSplatAccumulatedColor",
    "m_gaussianSplatAccumulationPass",
    "m_gaussianSplatTemporalSampleIndex",
    "m_gaussianSplatTemporalReset",
    "m_feedback_Buffer_Gpu",
    "m_feedback_Buffer_Cpu",
    "m_debugLineBufferCapture",
    "m_debugLineBufferDisplay",
    "m_linesVertexShader",
    "m_linesPixelShader",
    "m_cpuSideDebugLines",
    "m_linesInputLayout",
    "m_linesPipeline",
    "m_linesBindingLayout",
    "m_linesBindingSet",
    "m_feedbackData",
    "m_debugDeltaPathTree",
    "m_debugDeltaPathTree_Gpu",
    "m_debugDeltaPathTree_Cpu",
    "m_debugDeltaPathTreeSearchStack",
    "m_ptPipelineBaker",
]

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "caustica/editor/render/PathTracingRenderer.cpp"


def main():
    text = CPP.read_text(encoding="utf-8")
    for member in RENDER_MEMBERS:
        text = text.replace(f"m_app.{member}", member)
    # Fix duplicate include from prior extraction
    text = text.replace('#include "PathTracerApp.h"\n#include "PathTracerApp.h"', '#include "PathTracerApp.h"')
    CPP.write_text(text, encoding="utf-8")
    print(f"Updated {CPP}")


if __name__ == "__main__":
    main()
