#pragma once

#include <math/math.h>

#include <cstdint>

// Caustica-owned RTXDI configuration. NVIDIA RTXDI types are intentionally
// absent from this public header and are introduced only by the SDK adapter.
namespace caustica::rtxdi_config
{
enum class CheckerboardMode : uint32_t { Off = 0, Black = 1, White = 2 };
enum class DIResamplingMode : uint32_t { None = 0, Temporal, Spatial, TemporalAndSpatial, FusedSpatiotemporal };
enum class GIResamplingMode : uint32_t { None = 0, Temporal, Spatial, TemporalAndSpatial, FusedSpatiotemporal };
enum class PTResamplingMode : uint32_t { None = 0, Temporal, Spatial, TemporalAndSpatial };
enum class LocalLightSamplingMode : uint32_t { Uniform = 0, PowerRIS = 1, ReGIRRIS = 2 };
enum class DITemporalBiasCorrection : uint32_t { Off = 0, Basic = 1, Pairwise = 2, Raytraced = 3 };
enum class DISpatialBiasCorrection : uint32_t { Off = 0, Basic = 1, Pairwise = 2, Raytraced = 3 };
enum class GITemporalBiasCorrection : uint32_t { Off = 0, Basic = 1, Raytraced = 3 };
enum class GISpatialBiasCorrection : uint32_t { Off = 0, Basic = 1, Raytraced = 3 };
enum class PTReconnectionMode : uint16_t { FixedThreshold = 0, Footprint = 1 };
enum class ReGIRMode : uint32_t { Disabled = 0, Grid = 1, Onion = 2 };
enum class ReGIRSamplingMode : uint32_t { Uniform = 0, PowerRIS = 1 };

struct DIInitialSamplingParameters
{
    uint32_t numPrimaryLocalLightSamples = 8;
    uint32_t numPrimaryInfiniteLightSamples = 1;
    uint32_t numPrimaryEnvironmentSamples = 1;
    uint32_t numPrimaryBrdfSamples = 1;
    float brdfCutoff = 0.0001f;
    uint32_t enableInitialVisibility = 1;
    uint32_t environmentMapImportanceSampling = 1;
    LocalLightSamplingMode localLightSamplingMode = LocalLightSamplingMode::ReGIRRIS;
};

struct DITemporalResamplingParameters
{
    float temporalDepthThreshold = 0.1f;
    float temporalNormalThreshold = 0.5f;
    uint32_t maxHistoryLength = 20;
    DITemporalBiasCorrection temporalBiasCorrection = DITemporalBiasCorrection::Raytraced;
    uint32_t enablePermutationSampling = 1;
    float permutationSamplingThreshold = 0.9f;
    uint32_t enableBoilingFilter = 1;
    float boilingFilterStrength = 0.2f;
    uint32_t discardInvisibleSamples = 0;
};

struct DISpatialResamplingParameters
{
    float spatialDepthThreshold = 0.1f;
    float spatialNormalThreshold = 0.5f;
    DISpatialBiasCorrection spatialBiasCorrection = DISpatialBiasCorrection::Raytraced;
    uint32_t numSpatialSamples = 1;
    uint32_t numDisocclusionBoostSamples = 8;
    float spatialSamplingRadius = 32.0f;
    uint32_t discountNaiveSamples = 1;
};

struct DIShadingParameters
{
    uint32_t enableFinalVisibility = 1;
    uint32_t reuseFinalVisibility = 0;
    uint32_t finalVisibilityMaxAge = 4;
    float finalVisibilityMaxDistance = 16.0f;
    uint32_t enableDenoiserInputPacking = 0;
};

struct GITemporalResamplingParameters
{
    float depthThreshold = 0.1f;
    float normalThreshold = 0.6f;
    uint32_t enablePermutationSampling = 1;
    uint32_t maxHistoryLength = 10;
    uint32_t maxReservoirAge = 50;
    uint32_t enableBoilingFilter = 1;
    float boilingFilterStrength = 0.35f;
    uint32_t enableFallbackSampling = 1;
    GITemporalBiasCorrection temporalBiasCorrectionMode = GITemporalBiasCorrection::Basic;
};

struct GISpatialResamplingParameters
{
    float spatialDepthThreshold = 0.1f;
    float spatialNormalThreshold = 0.5f;
    uint32_t numSpatialSamples = 2;
    float spatialSamplingRadius = 32.0f;
    GISpatialBiasCorrection spatialBiasCorrectionMode = GISpatialBiasCorrection::Basic;
};

struct GIFinalShadingParameters
{
    uint32_t enableFinalVisibility = 1;
    uint32_t enableFinalMIS = 1;
};

struct PTInitialSamplingParameters
{
    uint32_t numInitialSamples = 1;
    uint32_t maxBounceDepth = 3;
    uint32_t maxRcVertexLength = 5;
};

struct PTReconnectionParameters
{
    float minConnectionFootprint = 0.02f;
    float minConnectionFootprintSigma = 0.2f;
    float minPdfRoughness = 0.1f;
    float minPdfRoughnessSigma = 0.01f;
    float roughnessThreshold = 0.1f;
    float distanceThreshold = 0.0f;
    PTReconnectionMode reconnectionMode = PTReconnectionMode::Footprint;
};

struct PTHybridShiftParameters
{
    uint32_t maxBounceDepth = 3;
    uint32_t maxRcVertexLength = 5;
};

struct PTTemporalResamplingParameters
{
    float depthThreshold = 0.1f;
    float normalThreshold = 0.6f;
    uint32_t enablePermutationSampling = 0;
    uint32_t maxHistoryLength = 8;
    uint32_t maxReservoirAge = 30;
    uint32_t enableFallbackSampling = 1;
    uint32_t enableVisibilityBeforeCombine = 0;
    uint32_t duplicationBasedHistoryReduction = 0;
    float historyReductionStrength = 0.8f;
};

struct BoilingFilterParameters
{
    uint32_t enableBoilingFilter = 1;
    float boilingFilterStrength = 0.2f;
};

struct PTSpatialResamplingParameters
{
    uint32_t numSpatialSamples = 1;
    uint32_t numDisocclusionBoostSamples = 8;
    uint32_t maxTemporalHistory = 8;
    uint32_t duplicationBasedHistoryReduction = 0;
    float samplingRadius = 32.0f;
    float normalThreshold = 0.6f;
    float depthThreshold = 0.1f;
};

struct ReGIRStaticParameters
{
    ReGIRMode Mode = ReGIRMode::Onion;
    uint32_t LightsPerCell = 512;
    caustica::math::uint3 GridSize = { 16, 16, 16 };
    uint32_t OnionDetailLayers = 5;
    uint32_t OnionCoverageLayers = 10;
};

struct ReGIRDynamicParameters
{
    float regirCellSize = 1.0f;
    caustica::math::float3 center = { 0.0f, 0.0f, 0.0f };
    ReGIRSamplingMode fallbackSamplingMode = ReGIRSamplingMode::PowerRIS;
    ReGIRSamplingMode presamplingMode = ReGIRSamplingMode::PowerRIS;
    float regirSamplingJitter = 1.0f;
    uint32_t regirNumBuildSamples = 8;
};

DIInitialSamplingParameters defaultDIInitialSampling();
DITemporalResamplingParameters defaultDITemporalResampling();
DISpatialResamplingParameters defaultDISpatialResampling();
DIShadingParameters defaultDIShading();
GITemporalResamplingParameters defaultGITemporalResampling();
GISpatialResamplingParameters defaultGISpatialResampling();
GIFinalShadingParameters defaultGIFinalShading();
PTInitialSamplingParameters defaultPTInitialSampling();
PTTemporalResamplingParameters defaultPTTemporalResampling();
PTReconnectionParameters defaultPTReconnection();
PTHybridShiftParameters defaultPTHybridShift();
BoilingFilterParameters defaultPTBoilingFilter();
PTSpatialResamplingParameters defaultPTSpatialResampling();
} // namespace caustica::rtxdi_config
