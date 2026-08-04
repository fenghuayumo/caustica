#pragma pack_matrix(row_major)

// 3DGUT primary raster (VS+PS): Unscented Transform projection + 3D ray-kernel opacity.
// Ported from vk_gaussian_splatting threedgut_* (CUDA/3DGUT paper path).

#include <shaders/FrameConstantBuffer.h>

#ifndef GAUSSIAN_SPLAT_HYBRID_SHADOWS
#define GAUSSIAN_SPLAT_HYBRID_SHADOWS 0
#endif

ConstantBuffer<GaussianSplatConstants> g_Const : register(b0);
StructuredBuffer<GaussianSplatData> t_Splats : register(t0);
Buffer<uint> t_SplatIndices : register(t1);
ByteAddressBuffer t_SplatRGBA : register(t2);
ByteAddressBuffer t_SplatSH : register(t3);
Texture2D<float> t_Depth : register(t4);

#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
RaytracingAccelerationStructure t_MeshBVH : register(t5);
#include <shaders/HybridGaussianShadow.hlsli>
#endif

static const uint kGaussianSplatFrustumCullingAtRaster = 2;
static const uint kGaussianSplatFormatFloat32 = 0;
static const uint kGaussianSplatFormatFloat16 = 1;
static const uint kGaussianSplatFormatUint8 = 2;
static const uint kGaussianSplatSortRandom = 1;
static const uint kGaussianSplatShScalarStride = 45;

static const float kGutD = 3.0f;
static const float kGutAlpha = 1.0f;
static const float kGutBeta = 2.0f;
static const float kGutLambda = 0.0f;
static const float kGutDelta = 1.73205080757f; // sqrt(alpha^2 * (D+kappa))
static const float kGutInImageMarginFactor = 0.1f;
static const float kGutCovarianceDilation = 0.3f;
static const float kGutAlphaThreshold = 0.01f;
static const float kGutMaxExtentFactor = 3.33f;
static const float kFragmentAlphaCullThreshold = 1.0f / 255.0f;
static const float kKernelMinResponse = 0.0113f;
static const float kAlphaClamp = 0.99f;

struct VertexOutput
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
    nointerpolation float3 splatPosition : TEXCOORD0;
    nointerpolation float3 splatScale : TEXCOORD1;
    nointerpolation float3 splatInvRotation0 : TEXCOORD2;
    nointerpolation float3 splatInvRotation1 : TEXCOORD3;
    nointerpolation float3 splatInvRotation2 : TEXCOORD4;
#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
    nointerpolation float3 worldCenter : TEXCOORD5;
#endif
};

float SrgbToLinear(float srgb)
{
    if (srgb <= 0.04045f)
        return srgb / 12.92f;
    if (srgb >= 1.0f)
        return srgb;
    return pow((srgb + 0.055f) / 1.055f, 2.4f);
}

float3 SrgbToLinear(float3 srgb)
{
    return float3(SrgbToLinear(srgb.r), SrgbToLinear(srgb.g), SrgbToLinear(srgb.b));
}

uint GaussianSplatFormatSize(uint format)
{
    return format == kGaussianSplatFormatFloat32 ? 4u : (format == kGaussianSplatFormatFloat16 ? 2u : 1u);
}

float LoadFormattedScalar(ByteAddressBuffer buffer, uint scalarIndex, uint format, bool signedRange)
{
    uint byteOffset = scalarIndex * GaussianSplatFormatSize(format);

    if (format == kGaussianSplatFormatFloat32)
        return asfloat(buffer.Load(byteOffset));

    if (format == kGaussianSplatFormatFloat16)
    {
        uint packed = buffer.Load(byteOffset & ~3u);
        uint halfBits = (packed >> ((byteOffset & 2u) * 8u)) & 0xffffu;
        return f16tof32(halfBits);
    }

    uint packed = buffer.Load(byteOffset & ~3u);
    float value = float((packed >> ((byteOffset & 3u) * 8u)) & 0xffu) / 255.0f;
    return signedRange ? value * 2.0f - 1.0f : value;
}

float4 LoadRGBA(uint splatIndex)
{
    uint base = splatIndex * 4u;
    return float4(
        LoadFormattedScalar(t_SplatRGBA, base + 0u, g_Const.rgbaFormat, false),
        LoadFormattedScalar(t_SplatRGBA, base + 1u, g_Const.rgbaFormat, false),
        LoadFormattedScalar(t_SplatRGBA, base + 2u, g_Const.rgbaFormat, false),
        LoadFormattedScalar(t_SplatRGBA, base + 3u, g_Const.rgbaFormat, false));
}

uint GaussianSplatHash32(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float Random01(uint3 seed)
{
    uint value = GaussianSplatHash32(seed.x ^ GaussianSplatHash32(seed.y ^ GaussianSplatHash32(seed.z)));
    return float(value & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float2 GetQuadCorner(uint vertexInSplat)
{
    switch (vertexInSplat)
    {
    case 0: return float2(-1.0, -1.0);
    case 1: return float2( 1.0, -1.0);
    case 2: return float2( 1.0,  1.0);
    case 3: return float2(-1.0, -1.0);
    case 4: return float2( 1.0,  1.0);
    default: return float2(-1.0,  1.0);
    }
}

// Matches vk_gaussian_splatting quatToMat3 (quat xyzw). Storage is wxyz.
float3x3 QuatWxyzToMat3(float4 wxyz)
{
    float w = wxyz.x;
    float x = wxyz.y;
    float y = wxyz.z;
    float z = wxyz.w;

    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;

    return float3x3(
        1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy),
        2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),
        2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy));
}

float3 LoadSH(uint splatIndex, uint coeff)
{
    uint base = splatIndex * kGaussianSplatShScalarStride + coeff * 3u;
    return float3(
        LoadFormattedScalar(t_SplatSH, base + 0u, g_Const.shFormat, true),
        LoadFormattedScalar(t_SplatSH, base + 1u, g_Const.shFormat, true),
        LoadFormattedScalar(t_SplatSH, base + 2u, g_Const.shFormat, true));
}

float3 FetchViewDependentRadiance(uint splatIndex, float3 objectViewDir)
{
    uint degree = min(g_Const.shDegree, 3u);
    if (degree == 0)
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

    float x = objectViewDir.x;
    float y = objectViewDir.y;
    float z = objectViewDir.z;

    float3 rgb = SH_C1 * (-LoadSH(splatIndex, 0) * y + LoadSH(splatIndex, 1) * z - LoadSH(splatIndex, 2) * x);

    if (degree >= 2)
    {
        float xx = x * x;
        float yy = y * y;
        float zz = z * z;
        float xy = x * y;
        float yz = y * z;
        float xz = x * z;

        rgb += (SH_C2[0] * xy) * LoadSH(splatIndex, 3)
            + (SH_C2[1] * yz) * LoadSH(splatIndex, 4)
            + (SH_C2[2] * (2.0f * zz - xx - yy)) * LoadSH(splatIndex, 5)
            + (SH_C2[3] * xz) * LoadSH(splatIndex, 6)
            + (SH_C2[4] * (xx - yy)) * LoadSH(splatIndex, 7);

        if (degree >= 3)
        {
            rgb += SH_C3[0] * LoadSH(splatIndex, 8) * (3.0f * x * x - y * y) * y
                + SH_C3[1] * LoadSH(splatIndex, 9) * x * y * z
                + SH_C3[2] * LoadSH(splatIndex, 10) * (4.0f * z * z - x * x - y * y) * y
                + SH_C3[3] * LoadSH(splatIndex, 11) * z * (2.0f * z * z - 3.0f * x * x - 3.0f * y * y)
                + SH_C3[4] * LoadSH(splatIndex, 12) * x * (4.0f * z * z - x * x - y * y)
                + SH_C3[5] * LoadSH(splatIndex, 13) * (x * x - y * y) * z
                + SH_C3[6] * LoadSH(splatIndex, 14) * x * (x * x - 3.0f * y * y);
        }
    }

    return rgb;
}

bool ProjectWorldToPixel(float3 worldPos, out float2 pixel)
{
    float4 clip = mul(float4(worldPos, 1.0f), g_Const.view.matWorldToClip);
    if (clip.w <= 1e-6f)
    {
        pixel = 0.0f;
        return false;
    }

    float invW = 1.0f / clip.w;
    pixel = clip.xy * invW * g_Const.view.clipToWindowScale + g_Const.view.clipToWindowBias;

    float2 resolution = g_Const.view.viewportSize;
    float2 tolMargin = resolution * kGutInImageMarginFactor;
    float2 local = pixel - g_Const.view.viewportOrigin;
    return local.x > -tolMargin.x && local.y > -tolMargin.y
        && local.x < resolution.x + tolMargin.x && local.y < resolution.y + tolMargin.y;
}

bool GutParticleProjection(
    float3 particlePosition,
    float3 particleScale,
    float3x3 particleRotation,
    out float2 particleProjCenter,
    out float3 particleProjCovariance)
{
    float2 projectedSigmaPoints[7];
    int numValidPoints = 0;

    float3 worldMean = mul(float4(particlePosition, 1.0f), g_Const.objectToWorld).xyz;
    if (ProjectWorldToPixel(worldMean, projectedSigmaPoints[0]))
        numValidPoints++;

    particleProjCenter = projectedSigmaPoints[0] * (kGutLambda / (kGutD + kGutLambda));
    float weightI = 1.0f / (2.0f * (kGutD + kGutLambda));

    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        float3 delta = kGutDelta * particleScale[i] * particleRotation[i];

        float3 worldPlus = mul(float4(particlePosition + delta, 1.0f), g_Const.objectToWorld).xyz;
        if (ProjectWorldToPixel(worldPlus, projectedSigmaPoints[i + 1]))
            numValidPoints++;
        particleProjCenter += weightI * projectedSigmaPoints[i + 1];

        float3 worldMinus = mul(float4(particlePosition - delta, 1.0f), g_Const.objectToWorld).xyz;
        if (ProjectWorldToPixel(worldMinus, projectedSigmaPoints[i + 1 + 3]))
            numValidPoints++;
        particleProjCenter += weightI * projectedSigmaPoints[i + 1 + 3];
    }

    if (numValidPoints == 0)
    {
        particleProjCenter = 0.0f;
        particleProjCovariance = 0.0f;
        return false;
    }

    float2 centeredPoint = projectedSigmaPoints[0] - particleProjCenter;
    float weight0 = kGutLambda / (kGutD + kGutLambda) + (1.0f - kGutAlpha * kGutAlpha + kGutBeta);
    particleProjCovariance = weight0 * float3(
        centeredPoint.x * centeredPoint.x,
        centeredPoint.x * centeredPoint.y,
        centeredPoint.y * centeredPoint.y);

    [unroll]
    for (int j = 0; j < 6; ++j)
    {
        centeredPoint = projectedSigmaPoints[j + 1] - particleProjCenter;
        particleProjCovariance += weightI * float3(
            centeredPoint.x * centeredPoint.x,
            centeredPoint.x * centeredPoint.y,
            centeredPoint.y * centeredPoint.y);
    }

    return true;
}

bool GutProjectedExtentConicOpacity(
    float3 covariance,
    inout float opacity,
    out float2 extent)
{
    float3 dilated = float3(covariance.x + kGutCovarianceDilation, covariance.y, covariance.z + kGutCovarianceDilation);
    float dilatedDet = dilated.x * dilated.z - dilated.y * dilated.y;
    if (dilatedDet == 0.0f)
    {
        extent = 0.0f;
        return false;
    }

    if (g_Const.mipSplattingAntialiasing != 0)
    {
        float covDet = covariance.x * covariance.z - covariance.y * covariance.y;
        float convolutionFactor = sqrt(max(0.000025f, covDet / dilatedDet));
        opacity *= convolutionFactor;
    }

    if (opacity < kGutAlphaThreshold)
    {
        extent = 0.0f;
        return false;
    }

    float maxPower = log(opacity / kGutAlphaThreshold);
    float extentFactor = min(kGutMaxExtentFactor, sqrt(2.0f * maxPower));
    float mid = 0.5f * (dilated.x + dilated.z);
    float lambda = mid + sqrt(max(0.01f, mid * mid - dilatedDet));
    float radius = extentFactor * sqrt(lambda);
    extent = min(extentFactor * sqrt(float2(dilated.x, dilated.z)), float2(radius, radius));
    return radius > 0.0f;
}

void ParticleCanonicalRay(
    float3 rayOrigin,
    float3 rayDirection,
    float3 particlePosition,
    float3 particleScale,
    float3x3 particleInvRotation,
    out float3 particleRayOrigin,
    out float3 particleRayDirection)
{
    float3 giscl = 1.0f / max(particleScale, float3(1e-8f, 1e-8f, 1e-8f));
    float3 gposc = rayOrigin - particlePosition;
    float3 gposcr = mul(gposc, particleInvRotation);
    particleRayOrigin = giscl * gposcr;

    float3 rayDirR = mul(rayDirection, particleInvRotation);
    float3 grdu = giscl * rayDirR;
    particleRayDirection = normalize(grdu);
}

float ParticleRayMinSquaredDistance(float3 particleRayOrigin, float3 particleRayDirection)
{
    float3 gcrod = cross(particleRayDirection, particleRayOrigin);
    return dot(gcrod, gcrod);
}

VertexOutput vs_main(uint vertexId : SV_VertexID)
{
    VertexOutput output = (VertexOutput)0;

    uint splatListIndex = vertexId / 6;
    uint vertexInSplat = vertexId - splatListIndex * 6;
    float2 corner = GetQuadCorner(vertexInSplat);

    if (splatListIndex >= g_Const.splatCount)
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    uint sourceSplatIndex = t_SplatIndices[splatListIndex];
    GaussianSplatData splat = t_Splats[sourceSplatIndex];
    float4 splatColorOpacity = LoadRGBA(sourceSplatIndex);

    if (splatColorOpacity.a < g_Const.alphaCullThreshold)
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    float4 worldCenter = mul(float4(splat.centerOpacity.xyz, 1.0f), g_Const.objectToWorld);
    float4 viewCenter = mul(worldCenter, g_Const.view.matWorldToView);
    float4 clipCenter = mul(worldCenter, g_Const.view.matWorldToClip);

    if (viewCenter.z <= 1e-4f || clipCenter.w <= 0.0f)
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    if (g_Const.frustumCulling == kGaussianSplatFrustumCullingAtRaster)
    {
        float clipLimit = (1.0f + max(g_Const.frustumDilation, 0.0f)) * clipCenter.w;
        if (abs(clipCenter.x) > clipLimit || abs(clipCenter.y) > clipLimit || clipCenter.z < 0.0f || clipCenter.z > clipCenter.w)
        {
            output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
            return output;
        }
    }

    float3 particleScale = max(splat.scale.xyz * max(g_Const.splatScale, 0.0f), float3(1e-8f, 1e-8f, 1e-8f));
    float3x3 particleRotation = QuatWxyzToMat3(splat.rotation);
    float3x3 particleInvRotation = transpose(particleRotation);

    float2 projCenter;
    float3 projCovariance;
    if (!GutParticleProjection(splat.centerOpacity.xyz, particleScale, particleRotation, projCenter, projCovariance))
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    float opacity = splatColorOpacity.a;
    float2 extent;
    if (!GutProjectedExtentConicOpacity(projCovariance, opacity, extent))
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    if (g_Const.screenSizeCulling != 0)
    {
        float pixelCoverage = 2.0f * max(extent.x, extent.y);
        if (pixelCoverage < max(g_Const.minPixelCoverage, 0.0f))
        {
            output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
            return output;
        }
    }

    float2 ndcXY = (projCenter - g_Const.view.clipToWindowBias) / g_Const.view.clipToWindowScale;
    float2 ndcExtent = extent / abs(g_Const.view.clipToWindowScale);
    float ndcZ = clipCenter.z / clipCenter.w;

    float3 color = splatColorOpacity.rgb * g_Const.tintColor;
    if (g_Const.shDegree > 0)
    {
        float3 objectViewDir = normalize(splat.centerOpacity.xyz - g_Const.cameraPositionObject.xyz);
        color += FetchViewDependentRadiance(sourceSplatIndex, objectViewDir);
    }

    output.position = float4(ndcXY + corner * ndcExtent, ndcZ, 1.0f);
    output.color = float4(SrgbToLinear(max(color, 0.0f)) * g_Const.brightness, opacity);
    output.splatPosition = splat.centerOpacity.xyz;
    output.splatScale = particleScale;
    output.splatInvRotation0 = particleInvRotation[0];
    output.splatInvRotation1 = particleInvRotation[1];
    output.splatInvRotation2 = particleInvRotation[2];
#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
    output.worldCenter = worldCenter.xyz;
#endif
    return output;
}

float4 ps_main(VertexOutput input, uint primitiveId : SV_PrimitiveID) : SV_Target0
{
    // Primary ray through this pixel (pinhole), then evaluate 3DGRT quadratic kernel in object space.
    float2 ndc = (input.position.xy - g_Const.view.clipToWindowBias) / g_Const.view.clipToWindowScale;
    float4 worldH = mul(float4(ndc, 0.0f, 1.0f), g_Const.view.matClipToWorldNoOffset);
    float3 worldFar = worldH.xyz / max(worldH.w, 1e-8f);
    float3 rayOriginWorld = g_Const.cameraPosition.xyz;
    float3 rayDirWorld = normalize(worldFar - rayOriginWorld);

    float3 rayOrigin = mul(float4(rayOriginWorld, 1.0f), g_Const.worldToObject).xyz;
    float3 rayDirection = normalize(mul(rayDirWorld, (float3x3)g_Const.worldToObject));

    float3x3 splatInvRotation = float3x3(
        input.splatInvRotation0,
        input.splatInvRotation1,
        input.splatInvRotation2);

    float3 particleRayOrigin;
    float3 particleRayDirection;
    ParticleCanonicalRay(
        rayOrigin,
        rayDirection,
        input.splatPosition,
        input.splatScale,
        splatInvRotation,
        particleRayOrigin,
        particleRayDirection);

    float rayDist = ParticleRayMinSquaredDistance(particleRayOrigin, particleRayDirection);
    float maxResponse = exp(-0.5f * rayDist);
    float density = input.color.a * g_Const.alphaScale;
    float opacity = min(kAlphaClamp, maxResponse * density);

    float alphaThreshold = max(g_Const.alphaCullThreshold, kFragmentAlphaCullThreshold);
    if (opacity <= alphaThreshold || maxResponse <= kKernelMinResponse)
        discard;

    if (g_Const.sortMode == kGaussianSplatSortRandom)
    {
        uint2 pixel = uint2(input.position.xy);
        uint sourceSplatIndex = t_SplatIndices[primitiveId / 2u];
        float randomValue = Random01(uint3(
            GaussianSplatHash32(pixel.x) ^ GaussianSplatHash32(pixel.y),
            sourceSplatIndex,
            g_Const.stochasticFrameIndex));
        if (randomValue >= saturate(opacity))
            discard;
        opacity = 1.0f;
    }

    if (g_Const.depthTest != 0)
    {
        uint width = 0;
        uint height = 0;
        t_Depth.GetDimensions(width, height);

        uint2 pixel = uint2(input.position.xy * float2(width, height) * g_Const.view.viewportSizeInv);
        if (pixel.x < width && pixel.y < height)
        {
            float sceneDepth = t_Depth.Load(int3(pixel, 0));
            if (sceneDepth > 0.0f && input.position.z < sceneDepth - 1e-6f)
                discard;
        }
    }

#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
    float shadow = 1.0f;
    if (g_Const.shadowsEnabled != 0 && g_Const.shadowStrength > 0.0f)
    {
        RayDesc shadowRay;
        shadowRay.Origin = input.worldCenter + g_Const.shadowDirectionToLight.xyz * max(g_Const.shadowDirectionToLight.w, 0.001f);
        shadowRay.Direction = g_Const.shadowDirectionToLight.xyz;
        shadowRay.TMin = 0.0f;
        shadowRay.TMax = g_Const.shadowRayTMax;

        uint2 pixel = uint2(input.position.xy);
        uint shadowSeed = HybridGaussian_MakeShadowSeed(
            shadowRay,
            pixel,
            g_Const.shadowFrameIndex,
            primitiveId);
        float visibility = HybridGaussian_TraceMeshShadowVisibility(
            t_MeshBVH,
            shadowRay,
            g_Const.shadowMode,
            g_Const.shadowSoftRadius,
            g_Const.shadowSoftSampleCount,
            shadowSeed);
        shadow = lerp(1.0f - saturate(g_Const.shadowStrength), 1.0f, visibility);
    }

    return float4(input.color.rgb * shadow, saturate(opacity));
#else
    return float4(input.color.rgb, saturate(opacity));
#endif
}
