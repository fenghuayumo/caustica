#ifndef __PATH_TRACER_SUBSURFACE_HLSLI__
#define __PATH_TRACER_SUBSURFACE_HLSLI__

// RTXCR uses the diffuse mean-free-path fit for the Burley profile.
#define USE_DIFFUSE_MEAN_FREE_PATH 1
#include "../ThirdParty/RTXCR/SubsurfaceScattering.hlsli"
#include "../ThirdParty/RTXCR/Transmission.hlsli"

inline float CausticaMax3(const float3 v)
{
    return max(v.x, max(v.y, v.z));
}

inline float CausticaSubsurfaceWeight(const ActiveBSDF bsdf)
{
    return saturate(bsdf.data.SubsurfaceWeight())
        * (1.0f - saturate(bsdf.data.Metallic()))
        * (1.0f - saturate(bsdf.data.SpecularTransmission()));
}

inline void CausticaMakeSubsurfaceMaterial(
    const ActiveBSDF bsdf, out RTXCR_SubsurfaceMaterialData material)
{
    material = RTXCR_CreateDefaultSubsurfaceMaterialData();
    // RTXCR's character preset uses the textured diffuse albedo as the SSS
    // transmission color. Keep OpenPBR's subsurface_color as an additional
    // volume tint; using it alone turns white-tinted character skin into wax
    // and discards authored details such as lips and freckles.
    material.transmissionColor = saturate(
        (float3)bsdf.data.Diffuse() * (float3)bsdf.data.SubsurfaceColor());
    material.scatteringColor = max((float3)bsdf.data.SubsurfaceRadiusScale(), 1e-4f);
    material.scale = max((float)bsdf.data.SubsurfaceRadius(), 1e-4f);
    material.g = clamp((float)bsdf.data.SubsurfaceAnisotropy(), -0.99f, 0.99f);
}

inline float CausticaSubsurfaceMaxRadius(
    const RTXCR_SubsurfaceMaterialData material,
    const bool isRtxcrEyePath, const float roughness)
{
    // Match RTXCR's character integrator. Once a path has crossed a
    // transmissive material, keep facial skin samples local and make the
    // rough eye-choroid/sclera material almost local. Without this branch the
    // one-unit skin radius smears the iris over the sclera and over-darkens
    // skin beneath the glasses.
    if (isRtxcrEyePath)
        return roughness >= 1.0f ? 1e-2f : 0.4f;

    const float fittedRadius = 8.0f * SSS_METERS_UNIT
        * material.scale * CausticaMax3(material.scatteringColor);
    // Match the RTXCR character demo's one-world-unit projection bound so
    // large imported scale values do not smear across facial features.
    return clamp(fittedRadius, 1e-4f, 1.0f);
}

inline bool CausticaSampleGlobalLight(
    const float3 position, const PathState path,
    inout UniformSampleSequenceGenerator sampleGenerator,
    out LightSample result)
{
    LightSampler lightSampler = Bridge::CreateLightSampler(
        path.GetPixelPos(), path.rayCone.getWidth(), path.GetSceneLength());

    const bool hasEnvironment = Bridge::HasEnvMap();
    const bool hasSceneLights = !lightSampler.IsEmpty();
    const float environmentBranchProbability = hasSceneLights ? 0.5f : 1.0f;

    // Baked environments are represented by many quad proxies in the global
    // light sampler. Picking only one proxy gives a correct but extremely
    // high-variance BSSRDF estimate. Sample the complete environment directly
    // and reserve the other branch for non-environment scene lights. Rejecting
    // environment proxies on that branch avoids double counting; retaining the
    // branch probability in the PDF keeps the mixture unbiased.
    if (hasEnvironment && (!hasSceneLights
        || sampleNext1D(sampleGenerator) < environmentBranchProbability))
    {
        const DistantLightSample envSample =
            Bridge::CreateEnvMapImportanceSampler().MIPDescentSample(
                sampleNext2D(sampleGenerator));
        if (envSample.Pdf <= 0.0f || !any(envSample.Le > 0.0f))
            return false;

        const float pdf = envSample.Pdf * environmentBranchProbability;
        result.Direction = envSample.Dir;
        result.Distance = kMaxSceneDistance;
        result.Li = envSample.Le / pdf;
        result.SolidAnglePdf = envSample.Pdf;
        result.LightIndex = CAUSTICA_INVALID_LIGHT_INDEX;
        result.SelectionPdf = environmentBranchProbability;
        result.LightSampleableByBSDF = true;
        result.FromLocalDistribution = false;
        return true;
    }

    if (!hasSceneLights)
        return false;

    float selectionPdf = 0.0f;
    const uint lightIndex = lightSampler.SampleGlobal(
        sampleNext1D(sampleGenerator), selectionPdf);
    if (selectionPdf <= 0.0f)
        return false;

    const PolymorphicLightInfoFull packedLight = lightSampler.LoadLight(lightIndex);
    const PolymorphicLightType lightType = PolymorphicLight::DecodeType(packedLight);
    if (hasEnvironment && (lightType == PolymorphicLightType::kEnvironment
        || lightType == PolymorphicLightType::kEnvironmentQuad))
        return false;

    const PolymorphicLightSample sampledLight = PolymorphicLight::CalcSample(
        packedLight, sampleNext2D(sampleGenerator), position);
    const float sceneLightBranchProbability = hasEnvironment
        ? (1.0f - environmentBranchProbability) : 1.0f;
    const float mixtureSelectionPdf = selectionPdf * sceneLightBranchProbability;
    const float pdf = sampledLight.SolidAnglePdf * mixtureSelectionPdf;
    if (pdf <= 0.0f || !any(sampledLight.Radiance > 0.0f))
        return false;

    const float3 toLight = sampledLight.Position - position;
    result.Distance = length(toLight);
    if (result.Distance <= 1e-7f)
        return false;

    result.Direction = toLight / result.Distance;
    result.Li = sampledLight.Radiance / pdf;
    result.SolidAnglePdf = sampledLight.SolidAnglePdf;
    result.LightIndex = lightIndex;
    result.SelectionPdf = mixtureSelectionPdf;
    result.LightSampleableByBSDF = sampledLight.LightSampleableByBSDF;
    result.FromLocalDistribution = false;
    return true;
}

inline float3 CausticaSubsurfaceVisibility(
    const float3 position, const float3 outwardFaceNormal,
    const LightSample lightSample, const PathState path,
    const WorkingContext workingContext)
{
    RayDesc ray;
    ray.Origin = ComputeRayOrigin(position, outwardFaceNormal);
    ray.Direction = lightSample.Direction;
    ray.TMin = 0.0f;
    ray.TMax = lightSample.Distance * 0.9985f;
    return Bridge::traceVisibilityRay(
        ray, path.rayCone, path.getVertexIndex(), workingContext.Debug);
}

inline bool CausticaLoadSubsurfaceHit(
    const RayDesc ray, const bool cullBackFaces,
    const uint initialInstanceIndex, const uint initialGeometryIndex,
    const bool requireSameGeometry, const uint initialMaterialID,
    const PathState path, const WorkingContext workingContext,
    out SurfaceData surface, out float hitT)
{
    TriangleHit triangleHit;
    if (!Bridge::traceSubsurfaceRay(ray, cullBackFaces, triangleHit, hitT))
        return false;
    if (triangleHit.instanceID.getInstanceIndex() != initialInstanceIndex)
        return false;
    if (requireSameGeometry
        && triangleHit.instanceID.getGeometryIndex() != initialGeometryIndex)
        return false;

    surface = Bridge::loadSurface(triangleHit, ray.Direction, path.rayCone,
        path.getVertexIndex(), path.GetPixelPos(), workingContext.Debug);
    return surface.shadingData.materialID == initialMaterialID;
}

inline float3 CausticaEvaluateBurleyDiffusion(
    const PathState path, const ShadingData shadingData, const ActiveBSDF bsdf,
    const uint instanceIndex, const uint geometryIndex,
    const RTXCR_SubsurfaceMaterialData material,
    const float maxRadius, inout UniformSampleSequenceGenerator sampleGenerator,
    const WorkingContext workingContext)
{
    RTXCR_SubsurfaceInteraction interaction = RTXCR_CreateSubsurfaceInteraction(
        shadingData.posW, shadingData.N, shadingData.T, shadingData.B);

    // RTXCR alternates between a surface-aligned disk and a camera-aligned
    // disk. The latter prevents the surface frame from stretching samples over
    // steeply curved facial features (nose, lips and eyelids).
    if (sampleNext1D(sampleGenerator) <= 0.5f)
    {
        const float3 cameraUp = normalize(g_Const.ptConsts.camera.CameraV);
        const float3 cameraDirection = normalize(g_Const.ptConsts.camera.DirectionW);
        interaction.normal = -cameraDirection;
        interaction.tangent = cameraUp;
        interaction.biTangent = normalize(cross(cameraUp, interaction.normal));
    }

    RTXCR_SubsurfaceSample subsurfaceSample;
    // RTXCR leaves single-scattering diffusion-profile correction disabled in
    // the Claire preset; enabling it shifts both the energy and the skin tint.
    RTXCR_EvalBurleyDiffusionProfile(material, interaction, maxRadius, false,
        sampleNext2D(sampleGenerator), subsurfaceSample);

    const float3 sampleOffset = subsurfaceSample.samplePosition - shadingData.posW;
    const float3 radialOffset = sampleOffset
        - interaction.normal * dot(sampleOffset, interaction.normal);
    if (length(radialOffset) >= maxRadius)
        return 0.0f;

    RayDesc projectionRay;
    projectionRay.Origin = subsurfaceSample.samplePosition;
    projectionRay.Direction = -interaction.normal;
    projectionRay.TMin = 0.0f;
    // The diffusion radius bounds the tangent-plane sample, not the distance
    // needed to project that sample back onto a curved surface. RTXCR traces
    // this ray without a radius-derived limit. In particular, its 0.01 eye
    // radius would otherwise leave only a 0.02 projection ray, causing most
    // sclera samples to miss the eyeball and return black.
    projectionRay.TMax = kMaxSceneDistance;

    SurfaceData sampleSurface;
    float projectionT;
    // RTXCR only accepts the front face of the exact geometry from which the
    // diffusion sample originated. Comparing just the material lets adjacent
    // eyelid/socket primitives sharing one skin material leak into each other.
    if (!CausticaLoadSubsurfaceHit(projectionRay, true, instanceIndex,
        geometryIndex, true, shadingData.materialID, path, workingContext,
        sampleSurface, projectionT))
        return 0.0f;

    LightSample lightSample;
    if (!CausticaSampleGlobalLight(sampleSurface.shadingData.posW,
        path, sampleGenerator, lightSample))
        return 0.0f;

    const float NoL = dot(sampleSurface.shadingData.N, lightSample.Direction);
    if (NoL <= 0.0f)
        return 0.0f;

    const float3 visibility = CausticaSubsurfaceVisibility(
        sampleSurface.shadingData.posW, sampleSurface.shadingData.faceNCorrected,
        lightSample, path, workingContext);
    return RTXCR_EvalBssrdf(subsurfaceSample,
        lightSample.Li * visibility, NoL);
}

inline float3 CausticaEvaluateSingleScatteringTransmission(
    const PathState path, const ShadingData shadingData, const ActiveBSDF bsdf,
    const uint instanceIndex, const RTXCR_SubsurfaceMaterialData material,
    inout UniformSampleSequenceGenerator sampleGenerator, const WorkingContext workingContext)
{
    const RTXCR_SubsurfaceInteraction interaction = RTXCR_CreateSubsurfaceInteraction(
        shadingData.posW, shadingData.N, shadingData.T, shadingData.B);
    const RTXCR_SubsurfaceMaterialCoefficients coefficients =
        RTXCR_ComputeSubsurfaceMaterialCoefficients(material);
    const float3 refractedDirection = RTXCR_CalculateRefractionRay(
        interaction, sampleNext2D(sampleGenerator));

    RayDesc transmissionRay;
    transmissionRay.Origin = shadingData.computeNewRayOrigin(false);
    transmissionRay.Direction = refractedDirection;
    transmissionRay.TMin = 0.0f;
    // RTXCR traces through the complete closed object. The surface diffusion
    // radius is unrelated to its thickness: coupling the two reduced the
    // 0.01-radius sclera path to a 0.08 interior ray, so it almost never found
    // the back of the eyeball and lost all transmission energy.
    transmissionRay.TMax = kMaxSceneDistance;

    SurfaceData exitSurface;
    float thickness;
    if (!CausticaLoadSubsurfaceHit(transmissionRay, false, instanceIndex,
        0u, false, shadingData.materialID, path, workingContext,
        exitSurface, thickness))
        return 0.0f;

    const float3 exitOutwardNormal = -exitSurface.shadingData.N;
    const float3 exitOutwardFaceNormal = -exitSurface.shadingData.faceNCorrected;
    float3 radiance = 0.0f;

    LightSample boundaryLight;
    if (CausticaSampleGlobalLight(exitSurface.shadingData.posW,
        path, sampleGenerator, boundaryLight)
        && dot(exitOutwardNormal, boundaryLight.Direction) > 0.0f)
    {
        const float3 visibility = CausticaSubsurfaceVisibility(
            exitSurface.shadingData.posW, exitOutwardFaceNormal,
            boundaryLight, path, workingContext);
        const float3 boundary = RTXCR_EvaluateBoundaryTerm(
            shadingData.N, boundaryLight.Direction, refractedDirection,
            exitOutwardNormal, thickness, coefficients);
        // Entry direction is cosine sampled, cancelling its cosine/pdf term.
        radiance += boundaryLight.Li * visibility * boundary * RTXCR_PI;
    }

    // RTXCR's default is one evenly spaced interior sample, i.e. the midpoint
    // for a single sample. The HG phase function is importance sampled, so its
    // phase/pdf ratio cancels.
    const float distanceSample = 0.5f * thickness;
    const float3 scatteringPosition = transmissionRay.Origin
        + refractedDirection * distanceSample;
    const float3 scatteringDirection = RTXCR_SampleDirectionHenyeyGreenstein(
        sampleNext2D(sampleGenerator), material.g, refractedDirection);

    RayDesc scatteringRay;
    scatteringRay.Origin = scatteringPosition;
    scatteringRay.Direction = scatteringDirection;
    scatteringRay.TMin = 1e-5f;
    scatteringRay.TMax = kMaxSceneDistance;

    SurfaceData scatteringExitSurface;
    float scatteringExitT;
    if (CausticaLoadSubsurfaceHit(scatteringRay, false, instanceIndex,
        0u, false, shadingData.materialID, path, workingContext,
        scatteringExitSurface, scatteringExitT))
    {
        const float3 scatteringExitNormal = -scatteringExitSurface.shadingData.N;
        const float3 scatteringExitFaceNormal = -scatteringExitSurface.shadingData.faceNCorrected;
        LightSample scatteringLight;
        if (CausticaSampleGlobalLight(scatteringExitSurface.shadingData.posW,
            path, sampleGenerator, scatteringLight)
            && dot(scatteringExitNormal, scatteringLight.Direction) > 0.0f)
        {
            const float3 visibility = CausticaSubsurfaceVisibility(
                scatteringExitSurface.shadingData.posW, scatteringExitFaceNormal,
                scatteringLight, path, workingContext);
            const float3 singleScattering = RTXCR_EvaluateSingleScattering(
                scatteringLight.Direction, scatteringExitNormal,
                distanceSample + scatteringExitT, coefficients);
            radiance += scatteringLight.Li * visibility
                * singleScattering * thickness;
        }
    }

    return radiance;
}

inline float3 HandleSubsurfaceNEE(
    const PathState path, const ShadingData shadingData, const ActiveBSDF bsdf,
    const uint instanceIndex, const uint geometryIndex,
    inout UniformSampleSequenceGenerator sampleGenerator,
    const WorkingContext workingContext)
{
    const float weight = CausticaSubsurfaceWeight(bsdf);
    if (weight <= 0.0f || shadingData.mtl.isThinSurface() || bsdf.data.IsHair())
        return 0.0f;

    RTXCR_SubsurfaceMaterialData material;
    CausticaMakeSubsurfaceMaterial(bsdf, material);
    if (!any(material.transmissionColor > 1e-4f))
        return 0.0f;
    const float maxRadius = CausticaSubsurfaceMaxRadius(material,
        path.hasFlag(PathFlags::rtxcrEyePath), (float)bsdf.data.Roughness());
    float3 radiance = CausticaEvaluateBurleyDiffusion(path, shadingData, bsdf,
        instanceIndex, geometryIndex, material, maxRadius,
        sampleGenerator, workingContext);
    radiance += CausticaEvaluateSingleScatteringTransmission(path, shadingData, bsdf,
        instanceIndex, material, sampleGenerator, workingContext);
    // RTXCR's character preset normalizes the diffusion and microfacet UI
    // weights to 0.5/0.5. The matching microfacet factor is applied while the
    // BSDF is assembled; keep the spatial component on the other half here.
    return path.GetThp() * max(radiance, 0.0f) * (0.5f * weight);
}

inline bool CausticaIsSubsurfaceSurface(
    const ShadingData shadingData, const ActiveBSDF bsdf)
{
    return CausticaSubsurfaceWeight(bsdf) > 0.0f
        && !shadingData.mtl.isThinSurface()
        && !bsdf.data.IsHair();
}

#endif
