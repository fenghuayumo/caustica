#pragma pack_matrix(row_major)

#include <shaders/SampleConstantBuffer.h>

#ifndef GAUSSIAN_SPLAT_SORT_KEYS
#define GAUSSIAN_SPLAT_SORT_KEYS 0
#endif

#ifndef GAUSSIAN_SPLAT_HYBRID_SHADOWS
#define GAUSSIAN_SPLAT_HYBRID_SHADOWS 0
#endif

ConstantBuffer<GaussianSplatConstants> g_Const : register(b0);
StructuredBuffer<GaussianSplatData> t_Splats : register(t0);

static const uint kGaussianSplatFrustumCullingDisabled = 0;
static const uint kGaussianSplatFrustumCullingAtDistance = 1;
static const uint kGaussianSplatFrustumCullingAtRaster = 2;
static const uint kGaussianSplatFormatFloat32 = 0;
static const uint kGaussianSplatFormatFloat16 = 1;
static const uint kGaussianSplatFormatUint8 = 2;
static const uint kGaussianSplatProjectionEigen = 0;
static const uint kGaussianSplatProjectionConic = 1;
static const uint kGaussianSplatSortRandom = 1;
static const uint kGaussianSplatShScalarStride = 45;

#if GAUSSIAN_SPLAT_SORT_KEYS

RWBuffer<uint> u_SortKeys : register(u0);
RWBuffer<uint> u_SplatIndices : register(u1);
RWBuffer<uint> u_SortControl : register(u2);
RWBuffer<uint> u_DrawIndirectArgs : register(u3);

[numthreads(256, 1, 1)]
void cs_sort_keys(uint splatIndex : SV_DispatchThreadID)
{
    if (splatIndex >= g_Const.splatCount)
        return;

    GaussianSplatData splat = t_Splats[splatIndex];
    float4 worldCenter = mul(float4(splat.centerOpacity.xyz, 1.0f), g_Const.objectToWorld);
    // Depth sorting ignores sub-pixel jitter so the sorted index buffer can be reused.
    float4 clipCenter = mul(worldCenter, g_Const.view.matWorldToClipNoOffset);

    if (g_Const.frustumCulling == kGaussianSplatFrustumCullingAtDistance)
    {
        float4 viewCenter = mul(worldCenter, g_Const.view.matWorldToView);
        if (viewCenter.z <= 1e-4f || clipCenter.w <= 0.0f)
            return;

        float clipLimit = (1.0f + max(g_Const.frustumDilation, 0.0f)) * clipCenter.w;
        if (abs(clipCenter.x) > clipLimit || abs(clipCenter.y) > clipLimit || clipCenter.z < 0.0f || clipCenter.z > clipCenter.w)
            return;
    }

    float reverseZ = 0.0f;
    if (clipCenter.w > 0.0f)
        reverseZ = saturate(clipCenter.z / clipCenter.w);

    uint sortKey = asuint(reverseZ);
    u_SortKeys[splatIndex] = sortKey;

    if (g_Const.frustumCulling == kGaussianSplatFrustumCullingAtDistance)
    {
        uint visibleIndex;
        InterlockedAdd(u_SortControl[0], 1u, visibleIndex);
        u_SplatIndices[visibleIndex] = splatIndex;
        uint previousVertexCount;
        InterlockedAdd(u_DrawIndirectArgs[0], 6u, previousVertexCount);
    }
}

#else

Buffer<uint> t_SplatIndices : register(t1);
ByteAddressBuffer t_SplatRGBA : register(t2);
ByteAddressBuffer t_SplatSH : register(t3);
Texture2D<float> t_Depth : register(t4);

#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
RaytracingAccelerationStructure t_MeshBVH : register(t5);

#include <shaders/HybridGaussianShadow.hlsli>
#endif

struct VertexOutput
{
    float4 position : SV_Position;
    float2 fragPos : TEXCOORD0;
    nointerpolation float3 conic : TEXCOORD1;
    nointerpolation float4 color : COLOR0;
#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
    nointerpolation float3 worldCenter : TEXCOORD2;
#endif
};

static const float kSqrt8 = 2.8284271247461903f;
static const float kFragmentAlphaCullThreshold = 1.0f / 255.0f;

float SrgbToLinear(float srgb)
{
    return srgb <= 0.04045f ? srgb / 12.92f : pow(max((srgb + 0.055f) / 1.055f, 0.0f), 2.4f);
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

float3x3 LoadCovariance(GaussianSplatData splat)
{
    float3x3 covariance = float3x3(
        splat.covariance0.x, splat.covariance0.y, splat.covariance0.z,
        splat.covariance0.y, splat.covariance0.w, splat.covariance1.x,
        splat.covariance0.z, splat.covariance1.x, splat.covariance1.y);
    float3x3 objectToWorld = (float3x3)g_Const.objectToWorld;
    return mul(transpose(objectToWorld), mul(covariance, objectToWorld));
}

float3 ProjectCovariance(float3x3 cov3D, float4 viewCenter)
{
    float z = max(viewCenter.z, 1e-4f);
    float invZ2 = 1.0f / (z * z);
    float2 focal = float2(
        abs(g_Const.view.matViewToClip[0][0]) * g_Const.view.viewportSize.x * 0.5f,
        abs(g_Const.view.matViewToClip[1][1]) * g_Const.view.viewportSize.y * 0.5f);

    float3x3 J = float3x3(
        focal.x / z, 0.0f, -(focal.x * viewCenter.x) * invZ2,
        0.0f, focal.y / z, -(focal.y * viewCenter.y) * invZ2,
        0.0f, 0.0f, 0.0f);

    float3x3 W = transpose((float3x3)g_Const.view.matWorldToView);
    float3x3 T = mul(J, W);
    float3x3 cov2D = mul(mul(T, cov3D), transpose(T));

    return float3(cov2D[0][0], cov2D[0][1], cov2D[1][1]);
}

bool ComputeProjectedBasis(float3 cov2D, inout float opacity, out float2 basis1, out float2 basis2, out float3 conic)
{
    float detOrig = max(cov2D.x * cov2D.z - cov2D.y * cov2D.y, 1e-12f);

    cov2D.x += 0.3f;
    cov2D.z += 0.3f;

    float a = cov2D.x;
    float b = cov2D.y;
    float d = cov2D.z;
    float det = a * d - b * b;
    float traceOver2 = 0.5f * (a + d);
    float root = sqrt(max(0.1f, traceOver2 * traceOver2 - det));
    float eigenValue1 = traceOver2 + root;
    float eigenValue2 = traceOver2 - root;

    if (eigenValue2 <= 0.0f)
    {
        basis1 = 0.0f;
        basis2 = 0.0f;
        conic = 0.0f;
        return false;
    }

    if (g_Const.mipSplattingAntialiasing != 0)
        opacity *= sqrt(max(detOrig / max(det, 1e-12f), 0.0f));

    conic = float3(d, -b, a) / max(det, 1e-12f);

    if (g_Const.projectionMethod == kGaussianSplatProjectionConic)
    {
        float radius = g_Const.splatScale * min(kSqrt8 * sqrt(eigenValue1), 2048.0f);
        basis1 = float2(radius, 0.0f);
        basis2 = float2(0.0f, radius);
        return true;
    }

    float2 eigenVector1 = normalize(float2(abs(b) < 0.001f ? 1.0f : b, eigenValue1 - a));
    float2 eigenVector2 = float2(eigenVector1.y, -eigenVector1.x);

    basis1 = eigenVector1 * g_Const.splatScale * min(kSqrt8 * sqrt(eigenValue1), 2048.0f);
    basis2 = eigenVector2 * g_Const.splatScale * min(kSqrt8 * sqrt(eigenValue2), 2048.0f);
    conic = float3(1.0f, 0.0f, 1.0f);

    return true;
}

float3 LoadSH(uint splatIndex, uint coeff)
{
    uint base = splatIndex * kGaussianSplatShScalarStride + coeff * 3u;
    return float3(
        LoadFormattedScalar(t_SplatSH, base + 0u, g_Const.shFormat, true),
        LoadFormattedScalar(t_SplatSH, base + 1u, g_Const.shFormat, true),
        LoadFormattedScalar(t_SplatSH, base + 2u, g_Const.shFormat, true));
}

float3 FetchViewDependentRadiance(uint splatIndex, float3 worldViewDir)
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

    float x = worldViewDir.x;
    float y = worldViewDir.y;
    float z = worldViewDir.z;

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

    float2 basis1;
    float2 basis2;
    float3 conic;
    float opacity = splatColorOpacity.a;
    if (!ComputeProjectedBasis(ProjectCovariance(LoadCovariance(splat), viewCenter), opacity, basis1, basis2, conic))
    {
        output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        return output;
    }

    if (g_Const.screenSizeCulling != 0)
    {
        float pixelCoverage = 2.0f * max(length(basis1), length(basis2));
        if (pixelCoverage < max(g_Const.minPixelCoverage, 0.0f))
        {
            output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
            return output;
        }
    }

    float3 ndcCenter = clipCenter.xyz / clipCenter.w;
    float2 ndcOffset = (corner.x * basis1 + corner.y * basis2) * g_Const.view.viewportSizeInv * 2.0f;

    float3 color = splatColorOpacity.rgb * g_Const.tintColor;
    if (g_Const.shDegree > 0)
    {
        float3 worldViewDir = normalize(worldCenter.xyz - g_Const.cameraPosition.xyz);
        color += FetchViewDependentRadiance(sourceSplatIndex, worldViewDir);
    }

    float3 displayColor = max(color, 0.0f);

    output.position = float4(ndcCenter.xy + ndcOffset, ndcCenter.z, 1.0f);
    output.fragPos = g_Const.projectionMethod == kGaussianSplatProjectionConic
        ? corner.x * basis1 + corner.y * basis2
        : corner * kSqrt8;
    output.conic = conic;
    output.color = float4(SrgbToLinear(displayColor) * g_Const.brightness, opacity);
#if GAUSSIAN_SPLAT_HYBRID_SHADOWS
    output.worldCenter = worldCenter.xyz;
#endif

    return output;
}

float4 ps_main(VertexOutput input, uint primitiveId : SV_PrimitiveID) : SV_Target0
{
    float A = input.conic.x * input.fragPos.x * input.fragPos.x
        + 2.0f * input.conic.y * input.fragPos.x * input.fragPos.y
        + input.conic.z * input.fragPos.y * input.fragPos.y;
    if (A > 8.0f)
        discard;

    float opacity = exp(-0.5f * A) * input.color.a * g_Const.alphaScale;
    if (opacity <= max(g_Const.alphaCullThreshold, kFragmentAlphaCullThreshold))
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

#endif
