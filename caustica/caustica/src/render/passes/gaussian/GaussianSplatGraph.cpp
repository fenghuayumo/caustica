#include <render/passes/gaussian/GaussianSplatGraph.h>

#include <math/math.h>
#include <scene/SceneLightAccess.h>
#include <shaders/light_cb.h>
#include <shaders/FrameConstantBuffer.h>

#include <algorithm>
#include <cmath>
#include <vector>

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

    float shadowLightLuminance(const dm::float3& color)
    {
        return std::max(0.0f, color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f);
    }

    void fillGaussianSplatReceiverShadowLights(
        std::span<const scene::LightRenderProxy> lights,
        GaussianSplatRenderSettings& settings)
    {
        struct Candidate
        {
            GaussianSplatReceiverShadowLight light{};
            float importance = 0.0f;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(lights.size());

        for (const scene::LightRenderProxy& proxy : lights)
        {
            Candidate candidate;
            const int lightType = scene::getLightType(proxy);

            if (lightType == LightType_Environment)
            {
                const scene::EnvironmentLightData* environment = scene::tryGetEnvironmentLightData(proxy.data);
                if (environment == nullptr)
                    continue;

                const dm::float3 radiance(
                    std::max(proxy.color.x * environment->radianceScale.x, 0.0f),
                    std::max(proxy.color.y * environment->radianceScale.y, 0.0f),
                    std::max(proxy.color.z * environment->radianceScale.z, 0.0f));
                candidate.light.positionAndType = dm::float4(0.0f, 0.0f, 0.0f, float(LightType_Environment));
                // A Gaussian has no receiver normal. Use a stable upper-hemisphere
                // direction as an inexpensive approximation of environment occlusion.
                const float rotation = environment->rotation;
                const dm::float3 direction = normalize(dm::float3(std::sin(rotation), 0.75f, std::cos(rotation)));
                candidate.light.directionAndRange = dm::float4(direction.x, direction.y, direction.z, 0.0f);
                candidate.light.colorAndIntensity = dm::float4(radiance.x, radiance.y, radiance.z, 1.0f);
                candidate.importance = shadowLightLuminance(radiance);
            }
            else
            {
                LightConstants lightConstants{};
                scene::fillLightConstants(proxy, lightConstants);
                if (lightConstants.intensity <= 0.0f)
                    continue;

                const dm::float3 color(
                    std::max(lightConstants.color.x, 0.0f),
                    std::max(lightConstants.color.y, 0.0f),
                    std::max(lightConstants.color.z, 0.0f));
                const float range = lightConstants.angularSizeOrInvRange > 0.0f
                    ? 1.0f / lightConstants.angularSizeOrInvRange
                    : 0.0f;

                candidate.light.positionAndType = dm::float4(
                    lightConstants.position.x,
                    lightConstants.position.y,
                    lightConstants.position.z,
                    float(lightType));
                candidate.light.colorAndIntensity = dm::float4(
                    color.x, color.y, color.z, std::max(lightConstants.intensity, 0.0f));

                if (lightType == LightType_Directional)
                {
                    const dm::float3 directionToLight = normalize(-lightConstants.direction);
                    candidate.light.directionAndRange = dm::float4(
                        directionToLight.x, directionToLight.y, directionToLight.z, 0.0f);
                    candidate.light.shape = dm::float4(
                        std::max(lightConstants.angularSizeOrInvRange, 0.0f), 0.0f, 0.0f, 0.0f);
                }
                else if (lightType == LightType_Point || lightType == LightType_Spot)
                {
                    candidate.light.directionAndRange = dm::float4(
                        lightConstants.direction.x,
                        lightConstants.direction.y,
                        lightConstants.direction.z,
                        range);
                    candidate.light.shape = dm::float4(
                        std::max(lightConstants.radius, 0.0f),
                        lightType == LightType_Spot ? std::cos(lightConstants.innerAngle) : -1.0f,
                        lightType == LightType_Spot ? std::cos(std::abs(lightConstants.outerAngle)) : -1.0f,
                        0.0f);
                }
                else
                {
                    continue;
                }

                candidate.importance = shadowLightLuminance(color) * lightConstants.intensity;
            }

            if (candidate.importance > 0.0f)
                candidates.push_back(candidate);
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.importance > b.importance;
        });

        settings.shadowLightCount = uint32_t(std::min<size_t>(
            candidates.size(), GAUSSIAN_SPLAT_MAX_RECEIVER_SHADOW_LIGHTS));
        for (uint32_t i = 0; i < settings.shadowLightCount; ++i)
            settings.shadowLights[i] = candidates[i].light;
    }
}

void fillGaussianSplatShadowConstants(
    FrameConstants& constants,
    const PathTracerSettings& settings,
    const GaussianSplatBinding& primaryBinding,
    uint32_t frameIndex,
    const dm::float3& shadowDirectionToLight)
{
    const uint32_t gaussianSplatShadowMode = resolveGaussianSplatShadowMode(settings);
    const GaussianSplatPass* primaryGaussianSplatPass = primaryBinding.splatPass;
    constants.GaussianSplatShadowCount = (settings.EnableGaussianSplats
            && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
            && primaryGaussianSplatPass != nullptr
            && primaryGaussianSplatPass->getTopLevelAS() != nullptr)
        ? primaryGaussianSplatPass->getSplatCount()
        : 0;
    constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0
            && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
        ? 1u
        : 0u;
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
    constants.GaussianSplatShadowRayOffset = settings.GaussianSplatShadowRayOffset;
    constants.GaussianSplatShadowAlphaScale = settings.GaussianSplatAlphaScale;
    constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatShadowKernelMinResponse;
    constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(settings.GaussianSplatShadowKernelDegree, 0, 5));
    constants.GaussianSplatShadowAdaptiveClamp = settings.GaussianSplatShadowAdaptiveClamp ? 1u : 0u;
    constants.GaussianSplatShadowStrength = std::clamp(settings.GaussianSplatShadowStrength, 0.0f, 1.0f);
    constants.GaussianSplatShadowDirectionToLight = normalize(shadowDirectionToLight);
    constants.GaussianSplatShadowWorldToObject = primaryBinding.splatPass != nullptr
        ? inverse(primaryBinding.objectToWorld)
        : dm::float4x4::identity();
}

bool hasTemporalGaussianSplatNoise(const PathTracerSettings& settings)
{
    const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
    const bool stochasticSoftShadows = settings.EnableGaussianSplats
        && resolveGaussianSplatShadowMode(settings) == GAUSSIAN_SPLAT_SHADOWS_SOFT
        && settings.GaussianSplatShadowStrength > 0.0f
        && settings.GaussianSplatShadowSoftRadius > 0.0f;
    return stochasticSplats || stochasticSoftShadows;
}

bool needsTemporalGaussianSplatsBeforeAA(const PathTracerSettings& settings)
{
    // Stochastic opacity must be rendered before the main temporal pass. Sorted
    // splats use alpha compositing after AA, so their soft-shadow noise is handled
    // by the Gaussian-owned accumulation pass instead.
    const bool stochasticSplats = settings.EnableGaussianSplats
        && settings.GaussianSplatSortingMode == 1;
    return stochasticSplats
        && settings.GaussianSplatApplyToneMapping
        && (!settings.RealtimeMode || settings.RealtimeAA == 1);
}

bool needsGaussianSplatsCompositePass(const PathTracerSettings& settings)
{
    if (!settings.EnableGaussianSplats)
        return false;

    return !needsTemporalGaussianSplatsBeforeAA(settings);
}

bool needsGaussianSplatTemporalAccumulate(const PathTracerSettings& settings)
{
    return hasTemporalGaussianSplatNoise(settings)
        && settings.GaussianSplatApplyToneMapping
        && needsGaussianSplatsCompositePass(settings);
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
    renderSettings.depthBias = std::max(settings.GaussianSplatDepthBias, 0.0f);
    renderSettings.depthEdgeDilation = settings.GaussianSplatDepthEdgeDilation;
    renderSettings.sortingMode = settings.GaussianSplatSortingMode == 1
        ? GaussianSplatSortMode::StochasticSplats
        : GaussianSplatSortMode::GpuSort;
    renderSettings.renderTarget = inputs.renderTarget;
    renderSettings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(
        std::clamp(settings.GaussianSplatFrustumCulling, 0, 2));
    renderSettings.primaryMethod = settings.GaussianSplatPrimaryMethod == 0
        ? GaussianSplatPrimaryMethod::GS
        : GaussianSplatPrimaryMethod::GUT;
    renderSettings.projectionMethod = static_cast<GaussianSplatProjectionMethod>(
        std::clamp(settings.GaussianSplatProjectionMethod, 0, 1));
    renderSettings.shFormat = static_cast<GaussianSplatStorageFormat>(std::clamp(settings.GaussianSplatSHFormat, 0, 2));
    renderSettings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(std::clamp(settings.GaussianSplatRGBAFormat, 0, 2));
    renderSettings.screenSizeCulling = settings.GaussianSplatScreenSizeCulling;
    renderSettings.mipSplattingAntialiasing = settings.GaussianSplatMipAntialiasing;
    renderSettings.referenceGammaCompositing = settings.GaussianSplatReferenceGammaCompositing
        && renderSettings.sortingMode == GaussianSplatSortMode::GpuSort
        && renderSettings.renderTarget == GaussianSplatRenderTarget::ProcessedOutputColor;
    renderSettings.covarianceDilation = std::clamp(settings.GaussianSplatCovarianceDilation, 0.0f, 2.0f);
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
    renderSettings.shadowRayOffset = settings.GaussianSplatShadowRayOffset;
    renderSettings.shadowSoftRadius = settings.GaussianSplatShadowSoftRadius;
    renderSettings.shadowSoftSampleCount = clampGaussianSplatSoftShadowSamples(settings.GaussianSplatShadowSoftSampleCount);
    renderSettings.shadowFrameIndex = uint32_t(inputs.frameIndex & 0xffffffffu);
    renderSettings.frustumDilation = settings.GaussianSplatFrustumDilation;
    renderSettings.minPixelCoverage = settings.GaussianSplatMinPixelCoverage;
    renderSettings.shadowDirectionToLight = resolveGaussianSplatShadowDirection(inputs.lights);
    fillGaussianSplatReceiverShadowLights(inputs.lights, renderSettings);

    if (renderSettings.renderTarget == GaussianSplatRenderTarget::LdrColor)
        renderSettings.stochasticFrameIndex = 0u;
    else if (stochasticSplats && settings.RealtimeMode)
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
