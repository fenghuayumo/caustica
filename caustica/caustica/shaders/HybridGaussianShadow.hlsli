#ifndef __HYBRID_GAUSSIAN_SHADOW_HLSLI__
#define __HYBRID_GAUSSIAN_SHADOW_HLSLI__

#include <shaders/SubInstanceData.h>
#include <shaders/PathTracer/Materials/StandardMaterial.h>

#ifndef GAUSSIAN_SPLAT_SHADOWS_DISABLED
#define GAUSSIAN_SPLAT_SHADOWS_DISABLED 0
#define GAUSSIAN_SPLAT_SHADOWS_HARD 1
#define GAUSSIAN_SPLAT_SHADOWS_SOFT 2
#endif

static const float kGaussianShadowExtent = 8.0f;

uint HybridGaussian_Hash32(uint value)
{
    value ^= value >> 16;
    value *= 0x21f0aaad;
    value ^= value >> 15;
    value *= 0xf35a2d97;
    value ^= value >> 15;
    return value;
}

uint HybridGaussian_HashCombine(uint seed, uint value)
{
    return seed ^ (HybridGaussian_Hash32(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

float HybridGaussian_HashToFloat(uint hash)
{
    return (hash >> 8) / float(1 << 24);
}

uint HybridGaussian_MakeShadowSeed(RayDesc ray, uint2 pixel, uint sampleIndex, uint salt)
{
    uint seed = HybridGaussian_Hash32(pixel.x ^ HybridGaussian_Hash32(pixel.y));
    seed = HybridGaussian_HashCombine(seed, asuint(ray.Origin.x));
    seed = HybridGaussian_HashCombine(seed, asuint(ray.Origin.y));
    seed = HybridGaussian_HashCombine(seed, asuint(ray.Origin.z));
    seed = HybridGaussian_HashCombine(seed, sampleIndex);
    seed = HybridGaussian_HashCombine(seed, salt);
    return seed;
}

void HybridGaussian_BuildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

RayDesc HybridGaussian_JitterShadowRay(RayDesc ray, float softRadius, uint seed)
{
    RayDesc jitteredRay = ray;

    if (softRadius <= 0.0f)
        return jitteredRay;

    float3 direction = normalize(ray.Direction);
    float3 tangent;
    float3 bitangent;
    HybridGaussian_BuildOrthonormalBasis(direction, tangent, bitangent);

    float u0 = HybridGaussian_HashToFloat(HybridGaussian_HashCombine(seed, 0));
    float u1 = HybridGaussian_HashToFloat(HybridGaussian_HashCombine(seed, 1));
    float radius = sqrt(u0) * softRadius;
    float angle = u1 * 6.28318530718f;
    float2 disk = float2(cos(angle), sin(angle)) * radius;

    jitteredRay.Direction = normalize(direction + tangent * disk.x + bitangent * disk.y);
    return jitteredRay;
}

RayDesc HybridGaussian_TransformRay(RayDesc ray, float4x4 transform)
{
    RayDesc transformedRay = ray;
    transformedRay.Origin = mul(float4(ray.Origin, 1.0f), transform).xyz;
    transformedRay.Direction = mul(float4(ray.Direction, 0.0f), transform).xyz;
    return transformedRay;
}

float3 HybridGaussian_RotateToCanonical(float3 value, float4 rotationWxyz)
{
    const float w = rotationWxyz.x;
    const float x = rotationWxyz.y;
    const float y = rotationWxyz.z;
    const float z = rotationWxyz.w;

    const float r00 = 1.0f - 2.0f * (y * y + z * z);
    const float r01 = 2.0f * (x * y - w * z);
    const float r02 = 2.0f * (x * z + w * y);
    const float r10 = 2.0f * (x * y + w * z);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float r12 = 2.0f * (y * z - w * x);
    const float r20 = 2.0f * (x * z - w * y);
    const float r21 = 2.0f * (y * z + w * x);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);

    // R^T transforms model-space vectors into the splat's canonical axes.
    return float3(
        r00 * value.x + r10 * value.y + r20 * value.z,
        r01 * value.x + r11 * value.y + r21 * value.z,
        r02 * value.x + r12 * value.y + r22 * value.z);
}

float HybridGaussian_ParticleRayMaxKernelResponse(float grayDist, uint kernelDegree)
{
    grayDist = max(grayDist, 0.0f);

    switch (kernelDegree)
    {
    case 5:
        return exp(-0.0185185185185f * grayDist * grayDist * sqrt(grayDist));
    case 4:
        return exp(-0.0555555555556f * grayDist * grayDist);
    case 3:
        return exp(-0.166666666667f * grayDist * sqrt(grayDist));
    case 1:
        return exp(-1.5f * sqrt(grayDist));
    case 0:
        return max(1.0f - 0.329630334487f * sqrt(grayDist), 0.0f);
    default:
        return exp(-0.5f * grayDist);
    }
}

bool HybridGaussian_IntersectSplat(
    RayDesc ray,
    GaussianSplatData splat,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
    float kernelMinResponse,
    uint kernelDegree,
    out float hitT,
    out float alpha)
{
    hitT = 0.0f;
    alpha = 0.0f;

    if (splat.centerOpacity.w <= alphaThreshold)
        return false;

    // Transform the ray into the Gaussian particle's canonical unit space.
    // particle's canonical unit space. Inverting the covariance with an
    // absolute determinant epsilon rejects ordinary millimetre-scale splats
    // because det(covariance) naturally falls far below 1e-12.
    const float3 safeScale = max(
        abs(splat.scale.xyz) * max(splatScale, 1e-4f),
        float3(1e-8f, 1e-8f, 1e-8f));
    const float4 normalizedRotation = normalize(splat.rotation);
    const float3 localOrigin = HybridGaussian_RotateToCanonical(
        ray.Origin - splat.centerOpacity.xyz,
        normalizedRotation) / safeScale;
    const float3 localDir = HybridGaussian_RotateToCanonical(
        ray.Direction,
        normalizedRotation) / safeScale;

    const float a = dot(localDir, localDir);
    const float b = 2.0f * dot(localDir, localOrigin);
    if (a <= 0.0f || !isfinite(a))
        return false;

    float t = -0.5f * b / a;
    if (t <= ray.TMin || t >= ray.TMax)
        return false;

    // Avoid subtracting huge squared terms for tiny splats far from the ray
    // origin. The residual vector retains the transverse distance accurately.
    const float3 closestPoint = mad(localDir, t, localOrigin);
    float grayDist = dot(closestPoint, closestPoint);
    float maxResponse = HybridGaussian_ParticleRayMaxKernelResponse(grayDist, kernelDegree);
    alpha = saturate(maxResponse * splat.centerOpacity.w * max(alphaScale, 0.0f));

    if (alpha <= alphaThreshold || maxResponse <= kernelMinResponse)
        return false;

    hitT = min(max(t, ray.TMin + 1e-4f), ray.TMax);
    return true;
}

// Shared Gaussian radiance tracing primitives. These are retained for Hybrid
// secondary rays (mesh reflection/refraction) and are not a primary renderer.
struct HybridGaussianRadianceResult
{
    float3 radiance;
    float transmittance;
    float firstHitT;
    uint hitCount;
};

float3 HybridGaussian_SrgbToLinear(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);
    return lerp(high, low, color <= 0.04045f);
}

float HybridGaussian_LoadShScalar(
    ByteAddressBuffer shCoefficients,
    uint scalarIndex,
    uint shFormat)
{
    const uint scalarSize = shFormat == 0u ? 4u : (shFormat == 1u ? 2u : 1u);
    const uint byteOffset = scalarIndex * scalarSize;
    if (shFormat == 0u)
        return asfloat(shCoefficients.Load(byteOffset));
    if (shFormat == 1u)
    {
        const uint packed = shCoefficients.Load(byteOffset & ~3u);
        const uint halfBits = (packed >> ((byteOffset & 2u) * 8u)) & 0xffffu;
        return f16tof32(halfBits);
    }

    const uint packed = shCoefficients.Load(byteOffset & ~3u);
    const float value = float((packed >> ((byteOffset & 3u) * 8u)) & 0xffu) / 255.0f;
    return value * 2.0f - 1.0f;
}

float3 HybridGaussian_LoadSh(
    ByteAddressBuffer shCoefficients,
    uint splatIndex,
    uint coefficientIndex,
    uint shFormat)
{
    const uint base = splatIndex * 45u + coefficientIndex * 3u;
    return float3(
        HybridGaussian_LoadShScalar(shCoefficients, base + 0u, shFormat),
        HybridGaussian_LoadShScalar(shCoefficients, base + 1u, shFormat),
        HybridGaussian_LoadShScalar(shCoefficients, base + 2u, shFormat));
}

float3 HybridGaussian_EvaluateViewDependentSh(
    ByteAddressBuffer shCoefficients,
    uint splatIndex,
    uint shDegree,
    uint shFormat,
    float3 objectViewDirection)
{
    const uint degree = min(shDegree, 3u);
    if (degree == 0u)
        return 0.0f;

    static const float SH_C1 = 0.4886025119029199f;
    static const float SH_C2[5] = {
        1.0925484f, -1.0925484f, 0.3153916f, -1.0925484f, 0.5462742f
    };
    static const float SH_C3[7] = {
        -0.5900435899266435f, 2.890611442640554f, -0.4570457994644658f,
        0.3731763325901154f, -0.4570457994644658f, 1.445305721320277f,
        -0.5900435899266435f
    };

    const float x = objectViewDirection.x;
    const float y = objectViewDirection.y;
    const float z = objectViewDirection.z;
    float3 radiance = SH_C1 * (
        -HybridGaussian_LoadSh(shCoefficients, splatIndex, 0u, shFormat) * y
        + HybridGaussian_LoadSh(shCoefficients, splatIndex, 1u, shFormat) * z
        - HybridGaussian_LoadSh(shCoefficients, splatIndex, 2u, shFormat) * x);

    if (degree >= 2u)
    {
        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float yz = y * z;
        const float xz = x * z;
        radiance += (SH_C2[0] * xy) * HybridGaussian_LoadSh(shCoefficients, splatIndex, 3u, shFormat)
            + (SH_C2[1] * yz) * HybridGaussian_LoadSh(shCoefficients, splatIndex, 4u, shFormat)
            + (SH_C2[2] * (2.0f * zz - xx - yy)) * HybridGaussian_LoadSh(shCoefficients, splatIndex, 5u, shFormat)
            + (SH_C2[3] * xz) * HybridGaussian_LoadSh(shCoefficients, splatIndex, 6u, shFormat)
            + (SH_C2[4] * (xx - yy)) * HybridGaussian_LoadSh(shCoefficients, splatIndex, 7u, shFormat);

        if (degree >= 3u)
        {
            radiance += SH_C3[0] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 8u, shFormat) * (3.0f * xx - yy) * y
                + SH_C3[1] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 9u, shFormat) * x * y * z
                + SH_C3[2] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 10u, shFormat) * (4.0f * zz - xx - yy) * y
                + SH_C3[3] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 11u, shFormat) * z * (2.0f * zz - 3.0f * xx - 3.0f * yy)
                + SH_C3[4] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 12u, shFormat) * x * (4.0f * zz - xx - yy)
                + SH_C3[5] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 13u, shFormat) * (xx - yy) * z
                + SH_C3[6] * HybridGaussian_LoadSh(shCoefficients, splatIndex, 14u, shFormat) * x * (xx - 3.0f * yy);
        }
    }

    return radiance;
}

bool HybridGaussian_BuildReceiverShadowRay(
    GaussianSplatReceiverShadowLight light,
    float3 worldPosition,
    float rayOffset,
    float defaultRayTMax,
    float defaultSoftRadius,
    out RayDesc ray,
    out float lightWeight,
    out float lightSoftRadius);

float3 HybridGaussian_TraceMeshShadowVisibility(
    RaytracingAccelerationStructure meshBVH,
    StructuredBuffer<SubInstanceData> subInstances,
    StructuredBuffer<StandardMaterialData> materials,
    uint materialCount,
    RayDesc ray,
    uint shadowMode,
    float softRadius,
    uint softSampleCount,
    uint seed);

HybridGaussianRadianceResult HybridGaussian_TraceRadiance(
    RaytracingAccelerationStructure gaussianBVH,
    RaytracingAccelerationStructure meshBVH,
    StructuredBuffer<SubInstanceData> subInstances,
    StructuredBuffer<StandardMaterialData> materials,
    uint materialCount,
    StructuredBuffer<GaussianSplatData> splats,
    ByteAddressBuffer shCoefficients,
    uint splatCount,
    uint shDegree,
    uint shFormat,
    RayDesc ray,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
    float kernelMinResponse,
    uint kernelDegree,
    uint useTlasInstances,
    uint primitiveCountPerSplat,
    uint maximumPassCount,
    float minimumTransmittance,
    float alphaClamp,
    float brightness,
    float3 tintColor,
    float4x4 objectToWorld,
    uint receiverShadowLightCount,
    GaussianSplatReceiverShadowLight receiverShadowLight,
    uint receiverShadowMode,
    float receiverShadowStrength,
    float receiverShadowRayOffset,
    float receiverShadowSoftRadius,
    uint receiverShadowFrameIndex)
{
    HybridGaussianRadianceResult result;
    result.radiance = 0.0f;
    result.transmittance = 1.0f;
    result.firstHitT = ray.TMax;
    result.hitCount = 0u;

    // Stochastic opacity is the Monte-Carlo form of front-to-back alpha
    // compositing: accept each splat with probability alpha and return the
    // nearest accepted one. Its expectation is exactly
    //   sum(T_i * alpha_i * radiance_i),
    // but it needs one BVH traversal, one SH fetch and at most one shadow ray.
    // This avoids truncating dense models after an arbitrary number of hits and
    // removes the large fixed arrays/register pressure from the path tracer.
    uint acceptedSplatIndex = 0xffffffffu;
    float acceptedHitT = ray.TMax;
    RayQuery<RAY_FLAG_NONE> rayQuery;
    rayQuery.TraceRayInline(gaussianBVH, RAY_FLAG_NONE, 0xff, ray);
    while (rayQuery.Proceed())
    {
        uint splatIndex = 0xffffffffu;
        bool proceduralProxy = false;
        bool triangleProxy = false;
        if (rayQuery.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            proceduralProxy = true;
            splatIndex = useTlasInstances != 0
                ? rayQuery.CandidateInstanceID()
                : rayQuery.CandidatePrimitiveIndex();
        }
        else if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            triangleProxy = true;
            const uint primitiveDivisor = max(primitiveCountPerSplat, 1u);
            splatIndex = useTlasInstances != 0
                ? rayQuery.CandidateInstanceID()
                : rayQuery.CandidatePrimitiveIndex() / primitiveDivisor;
        }

        if (splatIndex >= splatCount)
            continue;

        float hitT = acceptedHitT;
        float alpha = 0.0f;
        if (!HybridGaussian_IntersectSplat(
                ray, splats[splatIndex], splatScale,
                alphaThreshold, alphaScale, kernelMinResponse,
                kernelDegree, hitT, alpha)
            || hitT >= acceptedHitT)
        {
            continue;
        }

        alpha = min(saturate(alpha), saturate(alphaClamp));
        const uint opacitySeed = HybridGaussian_MakeShadowSeed(
            ray,
            uint2(splatIndex, asuint(hitT)),
            receiverShadowFrameIndex,
            0x47535254u);
        if (HybridGaussian_HashToFloat(HybridGaussian_Hash32(opacitySeed)) >= alpha)
            continue;

        acceptedSplatIndex = splatIndex;
        acceptedHitT = hitT;
        if (proceduralProxy)
        {
            // Committing the corrected maximum-density-plane distance shortens
            // traversal while still allowing a subsequently found nearer
            // accepted splat to replace this one.
            rayQuery.CommitProceduralPrimitiveHit(hitT);
        }
        else if (triangleProxy)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
        }
    }

    if (acceptedSplatIndex != 0xffffffffu)
    {
        const GaussianSplatData splat = splats[acceptedSplatIndex];
        float3 receiverShadow = 1.0f;
        if (receiverShadowLightCount != 0u
            && receiverShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
            && receiverShadowStrength > 0.0f)
        {
            const float3 worldCenter = mul(
                float4(splat.centerOpacity.xyz, 1.0f), objectToWorld).xyz;
            RayDesc shadowRay;
            float lightWeight;
            float lightSoftRadius;
            if (HybridGaussian_BuildReceiverShadowRay(
                    receiverShadowLight,
                    worldCenter,
                    receiverShadowRayOffset,
                    1.0e6f,
                    receiverShadowSoftRadius,
                    shadowRay,
                    lightWeight,
                    lightSoftRadius))
            {
                const uint shadowSeed = HybridGaussian_MakeShadowSeed(
                    shadowRay,
                    uint2(acceptedSplatIndex, asuint(acceptedHitT)),
                    receiverShadowFrameIndex,
                    0u);
                const float3 visibility = HybridGaussian_TraceMeshShadowVisibility(
                    meshBVH,
                    subInstances,
                    materials,
                    materialCount,
                    shadowRay,
                    receiverShadowMode,
                    lightSoftRadius,
                    1u,
                    shadowSeed);
                receiverShadow = lerp(1.0f - saturate(receiverShadowStrength), 1.0f, visibility);
            }
        }

        // Match vk_gaussian_splatting: SH is evaluated from the ray origin
        // toward the accepted particle center in object space.
        const float3 objectViewDirection = normalize(splat.centerOpacity.xyz - ray.Origin);
        float3 displayRadiance = splat.color.rgb * max(tintColor, 0.0f);
        displayRadiance += HybridGaussian_EvaluateViewDependentSh(
            shCoefficients, acceptedSplatIndex, shDegree, shFormat, objectViewDirection);
        result.radiance = HybridGaussian_SrgbToLinear(max(displayRadiance, 0.0f));
        result.radiance *= max(brightness, 0.0f) * receiverShadow;
        result.transmittance = 0.0f;
        result.firstHitT = acceptedHitT;
        result.hitCount = 1u;
    }

    return result;
}

float HybridGaussian_StochasticOpacitySample(RayDesc ray, uint splatIndex, float hitT, uint seed)
{
    float3 hitPosition = ray.Origin + ray.Direction * hitT;
    uint hash = HybridGaussian_HashCombine(seed, splatIndex);
    hash = HybridGaussian_HashCombine(hash, asuint(hitPosition.x));
    hash = HybridGaussian_HashCombine(hash, asuint(hitPosition.y));
    hash = HybridGaussian_HashCombine(hash, asuint(hitPosition.z));
    return HybridGaussian_HashToFloat(HybridGaussian_Hash32(hash));
}

bool HybridGaussian_AcceptStochasticSplat(
    RayDesc ray,
    uint splatIndex,
    GaussianSplatData splat,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
    float shadowStrength,
    float kernelMinResponse,
    uint kernelDegree,
    uint seed,
    out float hitT)
{
    float alpha = 0.0f;
    if (!HybridGaussian_IntersectSplat(
        ray,
        splat,
        splatScale,
        alphaThreshold,
        alphaScale,
        kernelMinResponse,
        kernelDegree,
        hitT,
        alpha))
    {
        return false;
    }

    return HybridGaussian_StochasticOpacitySample(ray, splatIndex, hitT, seed)
        < alpha * saturate(shadowStrength);
}

bool HybridGaussian_TraceGaussianShadow(
    RaytracingAccelerationStructure gaussianBVH,
    StructuredBuffer<GaussianSplatData> splats,
    uint splatCount,
    RayDesc ray,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
    float shadowStrength,
    float kernelMinResponse,
    uint kernelDegree,
    uint useTlasInstances,
    uint primitiveCountPerSplat,
    uint seed)
{
    if (splatCount == 0)
        return false;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
    rayQuery.TraceRayInline(gaussianBVH, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff, ray);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            uint splatIndex = useTlasInstances != 0
                ? rayQuery.CandidateInstanceID()
                : rayQuery.CandidatePrimitiveIndex();
            float hitT = 0.0f;
            if (splatIndex < splatCount
                && HybridGaussian_AcceptStochasticSplat(
                    ray,
                    splatIndex,
                    splats[splatIndex],
                    splatScale,
                    alphaThreshold,
                    alphaScale,
                    shadowStrength,
                    kernelMinResponse,
                    kernelDegree,
                    seed,
                    hitT))
            {
                rayQuery.CommitProceduralPrimitiveHit(hitT);
            }
        }
        else if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint primitiveDivisor = max(primitiveCountPerSplat, 1u);
            uint splatIndex = useTlasInstances != 0
                ? rayQuery.CandidateInstanceID()
                : rayQuery.CandidatePrimitiveIndex() / primitiveDivisor;
            float hitT = 0.0f;
            if (splatIndex < splatCount
                && HybridGaussian_AcceptStochasticSplat(
                    ray,
                    splatIndex,
                    splats[splatIndex],
                    splatScale,
                    alphaThreshold,
                    alphaScale,
                    shadowStrength,
                    kernelMinResponse,
                    kernelDegree,
                    seed,
                    hitT))
            {
                rayQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return rayQuery.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT
        || rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool HybridGaussian_TraceGaussianShadowMode(
    RaytracingAccelerationStructure gaussianBVH,
    StructuredBuffer<GaussianSplatData> splats,
    uint splatCount,
    RayDesc ray,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
    float shadowStrength,
    float kernelMinResponse,
    uint kernelDegree,
    uint useTlasInstances,
    uint primitiveCountPerSplat,
    uint shadowMode,
    float softRadius,
    float rayOffset,
    float4x4 worldToObject,
    uint seed)
{
    if (shadowMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
        return false;

    RayDesc shadowRay = ray;
    if (shadowMode == GAUSSIAN_SPLAT_SHADOWS_SOFT)
        shadowRay = HybridGaussian_JitterShadowRay(ray, softRadius, seed);

    float offset = max(rayOffset, 0.0f);
    if (offset > 0.0f)
    {
        shadowRay.Origin += normalize(shadowRay.Direction) * offset;
        shadowRay.TMax = max(shadowRay.TMin, shadowRay.TMax - offset);
    }

    RayDesc localShadowRay = HybridGaussian_TransformRay(shadowRay, worldToObject);

    return HybridGaussian_TraceGaussianShadow(
        gaussianBVH,
        splats,
        splatCount,
        localShadowRay,
        splatScale,
        alphaThreshold,
        alphaScale,
        shadowStrength,
        kernelMinResponse,
        kernelDegree,
        useTlasInstances,
        primitiveCountPerSplat,
        seed);
}

float3 HybridGaussian_TraceMeshShadow(
    RaytracingAccelerationStructure meshBVH,
    StructuredBuffer<SubInstanceData> subInstances,
    StructuredBuffer<StandardMaterialData> materials,
    uint materialCount,
    RayDesc ray)
{
    RayQuery<RAY_FLAG_NONE> rayQuery;
    rayQuery.TraceRayInline(meshBVH, RAY_FLAG_NONE, 0xff, ray);

    float3 transmittance = 1.0f;
    uint subInstanceCount;
    uint subInstanceStride;
    subInstances.GetDimensions(subInstanceCount, subInstanceStride);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;

        if (materialCount == 0u)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const uint subInstanceIndex = rayQuery.CandidateInstanceID() + rayQuery.CandidateGeometryIndex();
        if (subInstanceIndex >= subInstanceCount)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const SubInstanceData subInstance = subInstances[subInstanceIndex];
        if ((subInstance.FlagsAndAlphaInfo & SubInstanceData::Flags_ExcludeFromNEE) != 0)
            continue;
        if ((subInstance.FlagsAndAlphaInfo & SubInstanceData::Flags_CullVisibilityBackface) != 0
            && !rayQuery.CandidateTriangleFrontFace())
            continue;

        const uint materialIndex = subInstance.GlobalGeometryIndex_StandardMaterialDataIndex & 0xffffu;
        if (materialIndex >= materialCount)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const StandardMaterialData material = materials[materialIndex];
        const float transmission = saturate(max(material.TransmissionFactor, material.DiffuseTransmissionFactor));
        if (transmission <= 0.0f)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        // An ordinary closed glass shell contributes at both entry and exit.
        // Split its total material transmission across the two interfaces.
        float3 surfaceTransmission = saturate(material.BaseOrDiffuseColor.rgb * transmission);
        if ((material.Flags & StandardMaterialFlags_ThinSurface) == 0)
            surfaceTransmission = sqrt(surfaceTransmission);
        const float fresnelRatio = (material.IoR - 1.0f) / max(material.IoR + 1.0f, 1.0e-4f);
        const float fresnelF0 = fresnelRatio * fresnelRatio;
        surfaceTransmission *= 1.0f - saturate(fresnelF0 * transmission);
        transmittance *= surfaceTransmission;
        if (max(transmittance.r, max(transmittance.g, transmittance.b)) <= 1.0e-4f)
            return 0.0f;
    }

    return rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : transmittance;
}

// The shortest Gaussian axis approximates the receiver surface normal. Only
// sufficiently flat splats participate, since an isotropic splat has no stable normal.
float3 HybridGaussian_ReceiverNormal(
    GaussianSplatData splat,
    float3x3 worldToObject,
    float3 worldCenter,
    float3 cameraPosition)
{
    const float3 scale = max(splat.scale.xyz, 1.0e-8f);
    const float largestScale = max(scale.x, max(scale.y, scale.z));
    const float smallestScale = min(scale.x, min(scale.y, scale.z));
    if (smallestScale > largestScale * 0.85f)
        return 0.0f;

    const float3 axis = scale.x <= scale.y && scale.x <= scale.z ? float3(1.0f, 0.0f, 0.0f)
        : scale.y <= scale.z ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    const float3 q = splat.rotation.yzw;
    const float3 objectNormal = axis + 2.0f * cross(q, cross(q, axis) + splat.rotation.x * axis);
    float3 worldNormal = normalize(mul(worldToObject, objectNormal));
    if (dot(worldNormal, cameraPosition - worldCenter) < 0.0f)
        worldNormal = -worldNormal;
    return worldNormal;
}

float HybridGaussian_TraceMeshContact(
    RaytracingAccelerationStructure meshBVH,
    StructuredBuffer<SubInstanceData> subInstances,
    StructuredBuffer<StandardMaterialData> materials,
    uint materialCount,
    float3 worldCenter,
    float3 receiverNormal,
    float radius,
    float strength)
{
    if (radius <= 0.0f || strength <= 0.0f || dot(receiverNormal, receiverNormal) < 0.5f)
        return 0.0f;

    RayDesc ray;
    ray.Origin = worldCenter + receiverNormal * min(radius * 0.03f, 0.002f);
    ray.Direction = receiverNormal;
    ray.TMin = 0.0f;
    ray.TMax = radius;

    RayQuery<RAY_FLAG_NONE> rayQuery;
    rayQuery.TraceRayInline(meshBVH, RAY_FLAG_NONE, 0xff, ray);

    float nearestDistance = radius;
    float nearestOpacity = 0.0f;
    uint subInstanceCount;
    uint subInstanceStride;
    subInstances.GetDimensions(subInstanceCount, subInstanceStride);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;

        const uint subInstanceIndex = rayQuery.CandidateInstanceID() + rayQuery.CandidateGeometryIndex();
        if (subInstanceIndex >= subInstanceCount || materialCount == 0u)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const SubInstanceData subInstance = subInstances[subInstanceIndex];
        if ((subInstance.FlagsAndAlphaInfo & SubInstanceData::Flags_ExcludeFromNEE) != 0)
            continue;
        if ((subInstance.FlagsAndAlphaInfo & SubInstanceData::Flags_CullVisibilityBackface) != 0
            && !rayQuery.CandidateTriangleFrontFace())
            continue;

        const uint materialIndex = subInstance.GlobalGeometryIndex_StandardMaterialDataIndex & 0xffffu;
        if (materialIndex >= materialCount)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const StandardMaterialData material = materials[materialIndex];
        const float transmission = saturate(max(material.TransmissionFactor, material.DiffuseTransmissionFactor));
        if (transmission <= 0.0f)
        {
            rayQuery.CommitNonOpaqueTriangleHit();
            continue;
        }

        const float distance = rayQuery.CandidateTriangleRayT();
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            // Glass attenuates local ambient light without turning the contact black.
            nearestOpacity = lerp(1.0f, 0.65f, transmission);
        }
    }

    if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT
        && rayQuery.CommittedRayT() < nearestDistance)
    {
        nearestDistance = rayQuery.CommittedRayT();
        nearestOpacity = 1.0f;
    }

    const float proximity = 1.0f - smoothstep(0.0f, radius, nearestDistance);
    return saturate(strength) * nearestOpacity * proximity;
}

float HybridGaussian_ShadowLuminance(float3 color)
{
    return max(dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

bool HybridGaussian_BuildReceiverShadowRay(
    GaussianSplatReceiverShadowLight light,
    float3 worldPosition,
    float rayOffset,
    float defaultRayTMax,
    float defaultSoftRadius,
    out RayDesc ray,
    out float lightWeight,
    out float lightSoftRadius)
{
    const int lightType = int(round(light.positionAndType.w));
    const float intensity = max(light.colorAndIntensity.w, 0.0f);
    lightWeight = HybridGaussian_ShadowLuminance(light.colorAndIntensity.rgb) * intensity;
    lightSoftRadius = max(defaultSoftRadius, 0.0f);

    ray.Origin = worldPosition;
    ray.Direction = float3(0.0f, 1.0f, 0.0f);
    ray.TMin = 0.0f;
    ray.TMax = max(defaultRayTMax, 0.001f);

    if (lightWeight <= 0.0f)
        return false;

    if (lightType == LightType_Directional || lightType == LightType_Environment)
    {
        ray.Direction = normalize(light.directionAndRange.xyz);
        if (lightType == LightType_Directional)
            lightSoftRadius = max(lightSoftRadius, max(light.shape.x, 0.0f));
    }
    else if (lightType == LightType_Point || lightType == LightType_Spot)
    {
        const float3 toLight = light.positionAndType.xyz - worldPosition;
        const float distanceToLight = length(toLight);
        const float range = light.directionAndRange.w;
        if (distanceToLight <= 0.001f || (range > 0.0f && distanceToLight >= range))
            return false;

        ray.Direction = toLight / distanceToLight;
        ray.TMax = max(distanceToLight - max(rayOffset, 0.001f), 0.001f);

        float attenuation = rcp(max(distanceToLight * distanceToLight, 0.0001f));
        if (range > 0.0f)
        {
            const float normalizedDistance = distanceToLight / range;
            const float rangeFalloff = saturate(1.0f - normalizedDistance * normalizedDistance * normalizedDistance * normalizedDistance);
            attenuation *= rangeFalloff * rangeFalloff;
        }

        if (lightType == LightType_Spot)
        {
            const float3 lightToReceiver = -ray.Direction;
            const float coneCosine = dot(normalize(light.directionAndRange.xyz), lightToReceiver);
            const float outerCosine = min(light.shape.z, light.shape.y);
            const float innerCosine = max(light.shape.z, light.shape.y);
            attenuation *= innerCosine > outerCosine + 1.0e-5f
                ? smoothstep(outerCosine, innerCosine, coneCosine)
                : (coneCosine >= outerCosine ? 1.0f : 0.0f);
        }

        lightWeight *= attenuation;
        lightSoftRadius = max(lightSoftRadius, max(light.shape.x, 0.0f) / distanceToLight);
    }
    else
    {
        return false;
    }

    if (lightWeight <= 0.0f)
        return false;

    const float offset = max(rayOffset, 0.001f);
    ray.Origin += ray.Direction * offset;
    if (lightType == LightType_Directional || lightType == LightType_Environment)
        ray.TMax = max(ray.TMax - offset, 0.001f);
    return true;
}

float3 HybridGaussian_TraceMeshShadowVisibility(
    RaytracingAccelerationStructure meshBVH,
    StructuredBuffer<SubInstanceData> subInstances,
    StructuredBuffer<StandardMaterialData> materials,
    uint materialCount,
    RayDesc ray,
    uint shadowMode,
    float softRadius,
    uint softSampleCount,
    uint seed)
{
    if (shadowMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
        return 1.0f;

    if (shadowMode != GAUSSIAN_SPLAT_SHADOWS_SOFT)
        return HybridGaussian_TraceMeshShadow(meshBVH, subInstances, materials, materialCount, ray);

    uint sampleCount = min(max(softSampleCount, 1u), 16u);
    float3 visibleSamples = 0.0f;

    [loop]
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        RayDesc sampleRay = HybridGaussian_JitterShadowRay(
            ray,
            softRadius,
            HybridGaussian_HashCombine(seed, sampleIndex));
        visibleSamples += HybridGaussian_TraceMeshShadow(meshBVH, subInstances, materials, materialCount, sampleRay);
    }

    return visibleSamples / float(sampleCount);
}

#endif
