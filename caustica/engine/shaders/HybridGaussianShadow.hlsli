#ifndef __HYBRID_GAUSSIAN_SHADOW_HLSLI__
#define __HYBRID_GAUSSIAN_SHADOW_HLSLI__

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

float3x3 HybridGaussian_LoadCovariance(GaussianSplatData splat, float splatScale)
{
    float scale2 = splatScale * splatScale;
    return float3x3(
        splat.covariance0.x * scale2, splat.covariance0.y * scale2, splat.covariance0.z * scale2,
        splat.covariance0.y * scale2, splat.covariance0.w * scale2, splat.covariance1.x * scale2,
        splat.covariance0.z * scale2, splat.covariance1.x * scale2, splat.covariance1.y * scale2);
}

bool HybridGaussian_Invert3x3(float3x3 m, out float3x3 invM)
{
    float a = m[0][0], b = m[0][1], c = m[0][2];
    float d = m[1][0], e = m[1][1], f = m[1][2];
    float g = m[2][0], h = m[2][1], i = m[2][2];

    float A = e * i - f * h;
    float B = c * h - b * i;
    float C = b * f - c * e;
    float D = f * g - d * i;
    float E = a * i - c * g;
    float F = c * d - a * f;
    float G = d * h - e * g;
    float H = b * g - a * h;
    float I = a * e - b * d;

    float det = a * A + b * D + c * G;
    if (abs(det) < 1e-12f)
    {
        invM = 0.0f;
        return false;
    }

    float invDet = 1.0f / det;
    invM = float3x3(
        A * invDet, B * invDet, C * invDet,
        D * invDet, E * invDet, F * invDet,
        G * invDet, H * invDet, I * invDet);
    return true;
}

float HybridGaussian_QuadraticForm(float3x3 m, float3 v)
{
    return dot(v, mul(m, v));
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

    float3x3 invCov;
    if (!HybridGaussian_Invert3x3(HybridGaussian_LoadCovariance(splat, max(splatScale, 1e-4f)), invCov))
        return false;

    float3 localOrigin = ray.Origin - splat.centerOpacity.xyz;
    float3 localDir = ray.Direction;

    float a = HybridGaussian_QuadraticForm(invCov, localDir);
    float b = 2.0f * dot(localDir, mul(invCov, localOrigin));
    float c = HybridGaussian_QuadraticForm(invCov, localOrigin);
    if (abs(a) < 1e-12f)
        return false;

    float t = -0.5f * b / a;
    if (t <= ray.TMin || t >= ray.TMax)
        return false;

    float grayDist = max(a * t * t + b * t + c, 0.0f);
    float maxResponse = HybridGaussian_ParticleRayMaxKernelResponse(grayDist, kernelDegree);
    alpha = saturate(maxResponse * splat.centerOpacity.w * max(alphaScale, 0.0f));

    if (alpha <= alphaThreshold || maxResponse <= kernelMinResponse)
        return false;

    hitT = min(max(t, ray.TMin + 1e-4f), ray.TMax);
    return true;
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

    return HybridGaussian_StochasticOpacitySample(ray, splatIndex, hitT, seed) < alpha;
}

bool HybridGaussian_TraceGaussianShadow(
    RaytracingAccelerationStructure gaussianBVH,
    StructuredBuffer<GaussianSplatData> splats,
    uint splatCount,
    RayDesc ray,
    float splatScale,
    float alphaThreshold,
    float alphaScale,
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
        kernelMinResponse,
        kernelDegree,
        useTlasInstances,
        primitiveCountPerSplat,
        seed);
}

bool HybridGaussian_TraceMeshShadow(RaytracingAccelerationStructure meshBVH, RayDesc ray)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
    rayQuery.TraceRayInline(meshBVH, RAY_FLAG_FORCE_OPAQUE, 0xff, ray);

    while (rayQuery.Proceed())
    {
    }

    return rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float HybridGaussian_TraceMeshShadowVisibility(
    RaytracingAccelerationStructure meshBVH,
    RayDesc ray,
    uint shadowMode,
    float softRadius,
    uint softSampleCount,
    uint seed)
{
    if (shadowMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
        return 1.0f;

    if (shadowMode != GAUSSIAN_SPLAT_SHADOWS_SOFT)
        return HybridGaussian_TraceMeshShadow(meshBVH, ray) ? 0.0f : 1.0f;

    uint sampleCount = min(max(softSampleCount, 1u), 16u);
    float visibleSamples = 0.0f;

    [loop]
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        RayDesc sampleRay = HybridGaussian_JitterShadowRay(
            ray,
            softRadius,
            HybridGaussian_HashCombine(seed, sampleIndex));
        visibleSamples += HybridGaussian_TraceMeshShadow(meshBVH, sampleRay) ? 0.0f : 1.0f;
    }

    return visibleSamples / float(sampleCount);
}

#endif
