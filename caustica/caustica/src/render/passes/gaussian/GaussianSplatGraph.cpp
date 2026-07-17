#include <render/passes/gaussian/GaussianSplatGraph.h>

#include <math/math.h>
#include <scene/SceneLightAccess.h>
#include <shaders/light_cb.h>
#include <shaders/SampleConstantBuffer.h>

#include <algorithm>

namespace caustica::render
{

uint32_t resolveGaussianSplatShadowMode(const PathTracerSettings& settings)
{
    if (!settings.GaussianSplatShadows && settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
        return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

    const int requestedMode = settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
        ? GAUSSIAN_SPLAT_SHADOWS_HARD
        : settings.GaussianSplatShadowsMode;
    return uint32_t(std::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT));
}

uint32_t clampGaussianSplatSoftShadowSamples(int sampleCount)
{
    return uint32_t(std::clamp(sampleCount, 1, 16));
}

uint32_t clampGaussianSplatEmissionProxyCount(int proxyCount)
{
    return uint32_t(std::clamp(proxyCount, 0, 262144));
}

bool isGaussianSplatEmissionEnabled(const PathTracerSettings& settings)
{
    return settings.EnableGaussianSplats
        && settings.GaussianSplatAsEmitter
        && settings.GaussianSplatEmissionIntensity > 0.0f
        && settings.GaussianSplatEmissionMaxProxyCount > 0;
}

namespace
{
    constexpr float kGaussianSplatShadowKernelMinResponse = 0.0113f;
}

void fillGaussianSplatShadowConstants(
    SampleConstants& constants,
    const PathTracerSettings& settings,
    const GaussianSplatBinding& primaryBinding,
    uint32_t frameIndex)
{
    const uint32_t gaussianSplatShadowMode = resolveGaussianSplatShadowMode(settings);
    const GaussianSplatPass* primaryGaussianSplatPass = primaryBinding.splatPass;
    constants.GaussianSplatShadowCount = (settings.EnableGaussianSplats
            && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
            && primaryGaussianSplatPass != nullptr
            && primaryGaussianSplatPass->getTopLevelAS() != nullptr)
        ? primaryGaussianSplatPass->getSplatCount()
        : 0;
    constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0 ? 1u : 0u;
    constants.GaussianSplatShadowScale = settings.GaussianSplatScale;
    constants.GaussianSplatShadowAlphaThreshold = settings.GaussianSplatAlphaCullThreshold;
    constants.GaussianSplatShadowUseTLASInstances =
        (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->getShadowUsesTLASInstances()) ? 1u : 0u;
    constants.GaussianSplatShadowPrimitiveCountPerSplat =
        primaryGaussianSplatPass != nullptr ? primaryGaussianSplatPass->getShadowPrimitiveCountPerSplat() : 1u;
    constants.GaussianSplatShadowMode = constants.GaussianSplatShadowsEnabled != 0
        ? gaussianSplatShadowMode
        : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    constants.GaussianSplatShadowSoftRadius = settings.GaussianSplatShadowSoftRadius;
    constants.GaussianSplatShadowSoftSampleCount = clampGaussianSplatSoftShadowSamples(settings.GaussianSplatShadowSoftSampleCount);
    constants.GaussianSplatShadowFrameIndex = frameIndex;
    constants.GaussianSplatShadowRayOffset = settings.GaussianSplatRtxParticleShadowOffset;
    constants.GaussianSplatShadowAlphaScale = settings.GaussianSplatAlphaScale;
    constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatShadowKernelMinResponse;
    constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(settings.GaussianSplatRtxKernelDegree, 0, 5));
    constants.GaussianSplatShadowAdaptiveClamp = settings.GaussianSplatRtxAdaptiveClamp ? 1u : 0u;
    constants.GaussianSplatShadowWorldToObject = primaryBinding.splatPass != nullptr
        ? inverse(primaryBinding.objectToWorld)
        : dm::float4x4::identity();
}

bool needsStochasticGaussianSplatsBeforeAA(const PathTracerSettings& settings)
{
    const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
    return stochasticSplats && settings.RealtimeMode && settings.RealtimeAA == 1;
}

bool needsGaussianSplatsCompositePass(const PathTracerSettings& settings)
{
    if (!settings.EnableGaussianSplats)
        return false;

    const bool stochasticSplats = settings.GaussianSplatSortingMode == 1;
    const bool stochasticUsesMainTemporal = stochasticSplats
        && (!settings.RealtimeMode || settings.RealtimeAA == 1);
    return !stochasticUsesMainTemporal;
}

bool needsGaussianSplatStochasticAccumulate(const PathTracerSettings& settings)
{
    const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
    return stochasticSplats && needsGaussianSplatsCompositePass(settings);
}

bool needsGaussianSplatAccelBuild(const PathTracerSettings& settings)
{
    return settings.EnableGaussianSplats
        && resolveGaussianSplatShadowMode(settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
}

GaussianSplatRenderSettings buildGaussianSplatRenderSettings(const GaussianSplatFrameInputs& inputs)
{
    const PathTracerSettings& settings = inputs.settings;
    const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
    const uint32_t gaussianSplatShadowMode = resolveGaussianSplatShadowMode(settings);

    GaussianSplatRenderSettings renderSettings;
    renderSettings.enabled = settings.EnableGaussianSplats;
    renderSettings.depthTest = settings.GaussianSplatDepthTest;
    renderSettings.sortingMode = settings.GaussianSplatSortingMode == 1
        ? GaussianSplatSortMode::StochasticSplats
        : GaussianSplatSortMode::GpuSort;
    renderSettings.renderTarget = inputs.renderToOutputColor
        ? GaussianSplatRenderTarget::OutputColor
        : GaussianSplatRenderTarget::ProcessedOutputColor;
    renderSettings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(
        std::clamp(settings.GaussianSplatFrustumCulling, 0, 2));
    renderSettings.projectionMethod = GaussianSplatProjectionMethod::Eigen;
    renderSettings.shFormat = static_cast<GaussianSplatStorageFormat>(std::clamp(settings.GaussianSplatSHFormat, 0, 2));
    renderSettings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(std::clamp(settings.GaussianSplatRGBAFormat, 0, 2));
    renderSettings.screenSizeCulling = settings.GaussianSplatScreenSizeCulling;
    renderSettings.mipSplattingAntialiasing = settings.GaussianSplatMipAntialiasing;
    renderSettings.useAABBs = settings.GaussianSplatUseAABBs;
    renderSettings.useTLASInstances = settings.GaussianSplatUseTLASInstances;
    renderSettings.blasCompaction = settings.GaussianSplatBlasCompaction;
    renderSettings.splatScale = settings.GaussianSplatScale;
    renderSettings.alphaScale = settings.GaussianSplatAlphaScale;
    renderSettings.brightness = settings.GaussianSplatBrightness;
    renderSettings.tintColor = settings.GaussianSplatTintColor;
    renderSettings.alphaCullThreshold = settings.GaussianSplatAlphaCullThreshold;
    renderSettings.shadowsEnabled = gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    renderSettings.shadowMode = gaussianSplatShadowMode;
    renderSettings.shadowStrength = settings.GaussianSplatShadowStrength;
    renderSettings.shadowRayOffset = settings.GaussianSplatRtxParticleShadowOffset;
    renderSettings.shadowSoftRadius = settings.GaussianSplatShadowSoftRadius;
    renderSettings.shadowSoftSampleCount = clampGaussianSplatSoftShadowSamples(settings.GaussianSplatShadowSoftSampleCount);
    renderSettings.shadowFrameIndex = uint32_t(inputs.frameIndex & 0xffffffffu);
    renderSettings.frustumDilation = settings.GaussianSplatFrustumDilation;
    renderSettings.minPixelCoverage = settings.GaussianSplatMinPixelCoverage;
    renderSettings.shadowDirectionToLight = inputs.shadowDirectionToLight;

    if (stochasticSplats && settings.RealtimeMode)
        renderSettings.stochasticFrameIndex = uint32_t(inputs.temporalSampleIndex);
    else
        renderSettings.stochasticFrameIndex = uint32_t(inputs.sampleIndex >= 0
            ? uint32_t(inputs.sampleIndex)
            : uint32_t(inputs.frameIndex & 0xffffffffu));

    return renderSettings;
}

dm::float3 resolveGaussianSplatShadowDirection(std::span<const scene::LightRenderProxy> lights)
{
    for (const scene::LightRenderProxy& lightProxy : lights)
    {
        if (!caustica::scene::tryGetDirectionalLightData(lightProxy.data))
            continue;

        LightConstants lightConstants;
        caustica::scene::fillLightConstants(lightProxy, lightConstants);
        return -lightConstants.direction;
    }

    return dm::float3(0.0f, 1.0f, 0.0f);
}

} // namespace caustica::render
