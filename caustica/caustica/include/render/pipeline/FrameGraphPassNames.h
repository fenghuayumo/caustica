#pragma once

#include <render/core/PathTracerSettings.h>

namespace caustica::render
{

// Stable pass names used as executeAfter targets. Prefer these over empty fence nodes.

inline constexpr const char* kClearFrameTargetsPass = "ClearFrameTargets";
inline constexpr const char* kEnvMapUpdatePass = "EnvMapUpdate";
inline constexpr const char* kLightSamplingUpdateBeginPass = "LightSamplingUpdateBegin";
inline constexpr const char* kUploadSubInstanceDataPass = "UploadSubInstanceData";

// Last lighting prep pass — path trace / RTXDI begin depend on this.
inline constexpr const char* kLightingReadyPass = kUploadSubInstanceDataPass;

inline constexpr const char* kPathTraceLightingEndPass = "PathTraceLightingEnd";
inline constexpr const char* kVBufferExportPass = "VBufferExport";
inline constexpr const char* kGaussianSplatsAccelBuildPass = "GaussianSplatsAccelBuild";
inline constexpr const char* kMainPathTracePass = "MainPathTrace";
inline constexpr const char* kStablePlanesDebugVizPass = "StablePlanesDebugViz";

inline constexpr const char* kRtxdiPrepareLightsPass = "RtxdiPrepareLights";
inline constexpr const char* kRtxdiFillConstantsPass = "RtxdiFillConstants";
inline constexpr const char* kRtxdiGeneratePdfMipsPass = "RtxdiGeneratePdfMips";
inline constexpr const char* kRtxdiPresampleLightsPass = "RtxdiPresampleLights";
inline constexpr const char* kRtxdiPresampleEnvMapPass = "RtxdiPresampleEnvMap";
inline constexpr const char* kRtxdiPresampleReGIRPass = "RtxdiPresampleReGIR";

[[nodiscard]] inline const char* rtxdiBeginReadyPass(bool usingLightSampling)
{
    return usingLightSampling ? kRtxdiPresampleReGIRPass : kRtxdiFillConstantsPass;
}

inline constexpr const char* kRtxdiDIPass = "RtxdiDI";
inline constexpr const char* kRtxdiGIPass = "RtxdiGI";
inline constexpr const char* kRtxdiFusedDIGIFinalPass = "RtxdiFusedDIGIFinal";
inline constexpr const char* kRtxdiPTPass = "RtxdiPT";

[[nodiscard]] inline const char* rtxdiExecuteReadyPass(const PathTracerSettings& settings)
{
    static constexpr bool enableFusedDIGIFinal = true;
    const bool useDI = settings.actualUseReSTIRDI();
    const bool useGI = settings.actualUseReSTIRGI();
    const bool usePT = settings.actualUseReSTIRPT();
    const bool useFused = useDI && useGI && enableFusedDIGIFinal;

    if (usePT)
        return kRtxdiPTPass;
    if (useFused)
        return kRtxdiFusedDIGIFinalPass;
    if (useGI)
        return kRtxdiGIPass;
    if (useDI)
        return kRtxdiDIPass;
    return "MainPathTrace";
}

inline constexpr const char* kDenoiseSpecHitTPass = "DenoiseSpecHitT";
inline constexpr const char* kAvgLayerRadiancePass = "AvgLayerRadiance";

[[nodiscard]] inline const char* denoiseGuidesReadyPass()
{
    return kAvgLayerRadiancePass;
}

[[nodiscard]] inline const char* nrdPreparePassName(int planeIndex)
{
    static constexpr const char* kNames[] = {
        "NRD Prepare 0", "NRD Prepare 1", "NRD Prepare 2"
    };
    return kNames[planeIndex];
}

[[nodiscard]] inline const char* nrdRunPassName(int planeIndex)
{
    static constexpr const char* kNames[] = {
        "NRD Run 0", "NRD Run 1", "NRD Run 2"
    };
    return kNames[planeIndex];
}

[[nodiscard]] inline const char* nrdMergePassName(int planeIndex)
{
    static constexpr const char* kNames[] = {
        "NRD Merge 0", "NRD Merge 1", "NRD Merge 2"
    };
    return kNames[planeIndex];
}

[[nodiscard]] inline const char* nrdReadyPassName(int planeIndex)
{
    return nrdMergePassName(planeIndex);
}

} // namespace caustica::render
