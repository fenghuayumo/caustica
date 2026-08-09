#pragma once

namespace caustica::render
{

// Stable diagnostic names for GPU markers, telemetry, and graph validation.

inline constexpr const char* kUploadFrameConstantsPass = "UploadFrameConstants";
inline constexpr const char* kEnvMapUpdatePass = "EnvMapUpdate";
inline constexpr const char* kLightSamplingUpdateBeginPass = "LightSamplingUpdateBegin";
inline constexpr const char* kUploadSubInstanceDataPass = "UploadSubInstanceData";

// Last lighting prep pass — path trace / RTXDI begin depend on this.
// When there is no scene, lighting ready is the FrameConstants upload alone.
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

inline constexpr const char* kRtxdiDIPass = "RtxdiDI";
inline constexpr const char* kRtxdiGIPass = "RtxdiGI";
inline constexpr const char* kRtxdiFusedDIGIFinalPass = "RtxdiFusedDIGIFinal";
inline constexpr const char* kRtxdiPTPass = "RtxdiPT";

inline constexpr const char* kDenoiseSpecHitTPass = "DenoiseSpecHitT";
inline constexpr const char* kAvgLayerRadiancePass = "AvgLayerRadiance";

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
