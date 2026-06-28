/***************************************************************************
 # Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#ifndef __STF_SAMPLER_STATE_IMPL_HLSLI__
#define __STF_SAMPLER_STATE_IMPL_HLSLI__

// This file should be included by StochasticTextureFiltering.hlsli
int2 STF_NearestNeighbor2D(float2 coordinate)
{
    return int2(round(coordinate));
}

int3 STF_NearestNeighbor3D(float3 coordinate)
{
    return int3(round(coordinate));
}

// Takes the input coordinate and computes a probability based on the distance the float value is to the floor of itself
// The random value u is used to compare whether to increment the coordinate based on the probability to select a neighboring integer for sampling
int STF_StochasticLinear(float coordinate, inout float u)
{
    int s = int(floor(coordinate));

    // Compute the float remainder from the floor on the input value,
    // probabilityS is used as the probability that s input coordinate will be incremented
    const float probabilityS = coordinate - s;

    // Increment the input coordinate s if the random value u [0,1] is less than the probability to select an incremented coordinate
    if (u < probabilityS)
    {
        ++s;

        // Generates a new random number from u
        u /= probabilityS;
    }
    else 
    {
        // Generates a new random number from u
        u = (u - probabilityS) / (1 - probabilityS);
    }

    return s;
}

// Takes a 3D coordinate and computes a texel coordinate based on a random input [0,1]
int2 STF_StochasticBilinear(float2 st, inout float2 u)
{
    int2 res;
    res.x = STF_StochasticLinear(st.x, u.x);
    res.y = STF_StochasticLinear(st.y, u.y);
    return res;
}

// Takes a 3D coordinate and computes a texel coordinate based on a random input [0,1]
int3 STF_StochasticTrilinear(float3 st, float3 u)
{
    return int3(floor(st+u));
}

// Helper function to compute Bicubic weights
float4 STF_GetStochasticBicubicWeights(float t)
{
    const float t2 = t * t;

    float4 w;
    w.x = (1.f/6.f) * (-t*t2 + 3*t2 - 3*t + 1);
    w.y = (1.f/6.f) * (3*t*t2 - 6*t2 + 4);
    w.z = (1.f/6.f) * (-3*t*t2 + 3*t2 + 3*t + 1);
    w.w = (1.f/6.f) * t*t2;
    return w;
}

// Takes in bicubic weights to determine which texel in the bicubic footprint to select.
// Bicubic is a 4x4 texel area footprint and so we increment between [0-3]
int STF_StochasticLinear(float4 w, float u)
{
    float w_sum = w.x;

    if (u < w_sum)
    {
        return 0;
    }

    w_sum += w.y;

    if (u < w_sum)
    {
        return 1;
    }

    w_sum += w.z;

    if (u < w_sum)
    {
        return 2;
    }

    return 3;
}

int2 STF_StochasticBicubic(float2 st, float u, float u2)
{
    // Gather the bicubic weights for each coordinate
    float4 ws = STF_GetStochasticBicubicWeights(st.x - floor(st.x));
    float4 wt = STF_GetStochasticBicubicWeights(st.y - floor(st.y));

    // Sets a beggining texel location to offset from for the bicubic footprint
    int s0 = int(floor(st.x)) - 1;
    int t0 = int(floor(st.y)) - 1;

    // For each coordinate determine the texel location based on separate probabilities
    int s = STF_StochasticLinear(ws, u);
    int t = STF_StochasticLinear(wt, u2);

    return int2(s0 + s, t0 + t);
}

int3 STF_StochasticTricubic(float3 st, float3 u)
{
    // Gather the bicubic weights for each coordinate
    float4 ws = STF_GetStochasticBicubicWeights(st.x - floor(st.x));
    float4 wt = STF_GetStochasticBicubicWeights(st.y - floor(st.y));
    float4 wv = STF_GetStochasticBicubicWeights(st.z - floor(st.z));

    // Sets a beggining texel location to offset from for the bicubic footprint
    int3 st0 = int3(floor(st)) - 1;

    // For each coordinate determine the texel location based on separate probabilities
    int s = STF_StochasticLinear(ws, u.x);
    int t = STF_StochasticLinear(wt, u.y);
    int v = STF_StochasticLinear(wv, u.z);

    return st0 + int3(s, t, v);
}

float2 STF_BoxMullerTransform(float2 u)
{
    float2 r;
    float mag = sqrt(-2.0 * log(u.x));
    return mag * float2(cos(2.0 * STF_PI * u.y), sin(2.0 * STF_PI * u.y));
}

// Generates a 2D Gaussian by computing the Box-Muller 2D transform
int2 STF_StochasticGaussian2D(float  sigma, float2 st, float2 u)
{
    float2 offset = sigma * STF_BoxMullerTransform(u);
    return int2(round(st + offset));
}

// Generates a 3D Gaussian by computing the Box-Muller 2D transform and
// extending to the third dimension
int3 STF_StochasticGaussian3D(  float  sigma,
                                float3 st,
                                /*inout- TODO */ float3 u)
{
    float3 offset = sigma * float3(STF_BoxMullerTransform(u.xy), sqrt(-2.0 * log(u.z)) * cos(2.0 * STF_PI * u.z));
    return int3(round(st + offset));
}

float STF_BSplineCubic_PDF(float t)
{
    t = abs(t);
    if (t <= 1.0)
        return 0.5*t*t*t - t*t + 2.0/3.0;
    if (t <= 2.0)
        return -1.0/6.0*t*t*t + t*t - 2.0*t + 4.0/3.0;
    return 0;
}

float STF_Gaussian_PDF(float t, float sigma)
{
    return exp(-0.5 * t * t / (sigma * sigma));
}

// Method described in the stf paper
float STF_ComputeTextureLod_STF( uint2 dim,
                                 float4 textureGrads,
                                 float  minLod,
                                 float  maxLod,
                                 float  mipBias)
{
    const float dudx = dim.x * textureGrads.x;
    const float dvdx = dim.y * textureGrads.y;
    const float dudy = dim.x * textureGrads.z;
    const float dvdy = dim.y * textureGrads.w;

    float2 maxAxis = float2(dudy, dvdy);
    float2 minAxis = float2(dudx, dvdx);

    if (dot(minAxis, minAxis) > dot(maxAxis, maxAxis))
    {
        minAxis = float2(dudy, dvdy);
        maxAxis = float2(dudx, dvdx);
    }

    float minAxisLength = length(minAxis);
    const float maxAxisLength = length(maxAxis);

    float maxAnisotropy = 16;
    if ( minAxisLength > 0 && (minAxisLength * maxAnisotropy) < maxAxisLength)
    {
        float scale = maxAxisLength / (minAxisLength * maxAnisotropy);
        minAxisLength *= scale;
    }

    const float log2MinAxis = minAxisLength > 0.00001 ? log2(minAxisLength) : minLod;
    const float mipValue = clamp(log2MinAxis + mipBias, minLod, maxLod);
    return mipValue;
}

// Computation extracted from https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#LODCalculation
float STF_ComputeIsotropicLod_Cube(float3 uv,
                            uint   width,
                            float3 ddxUVW,
                            float3 ddyUVW,
                            float  minLod,
                            float  maxLod,
                            float  mipBias,
                            inout float  u)
{
    // Anisotropic is not supported for TextureCube so we compute isotropic of selected cube face
    const float dudx = width * ddxUVW.x;
    const float dvdx = width * ddxUVW.y;
    const float dwdx = width * ddxUVW.z;
    const float dudy = width * ddyUVW.x;
    const float dvdy = width * ddyUVW.y;
    const float dwdy = width * ddyUVW.z;
    float maxComponent = max(abs(uv.z), max(abs(uv.x), abs(uv.y)));
    float lengthX = 0;
    float lengthY = 0;
    if(maxComponent == abs(uv.x))
    {
        lengthX = sqrt(dvdx*dvdx + dwdx*dwdx);
        lengthY = sqrt(dvdy*dvdy + dwdy*dwdy);
    }
    else if(maxComponent == abs(uv.y))
    {
        lengthX = sqrt(dudx*dudx + dwdx*dwdx);
        lengthY = sqrt(dudy*dudy + dwdy*dwdy);
    }
    else
    {
        lengthX = sqrt(dudx*dudx + dvdx*dvdx);
        lengthY = sqrt(dudy*dudy + dvdy*dvdy);
    }
    const float log2MaxAxis = log2(max(lengthX, lengthY));
    int ilod = STF_StochasticLinear(log2MaxAxis + mipBias, u);
    return (uint)clamp(ilod, minLod, maxLod);
}

// Computation extracted from https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#LODCalculation
float STF_ComputeIsotropicLod_3D(float3 uv,
                            uint3  dim,
                            float3 ddxUVW,
                            float3 ddyUVW,
                            float  minLod,
                            float  maxLod,
                            float  mipBias,
                            float  u)
{
    // Anisotropic is not supported for Texture3D so we compute isotropic
    const float dudx = dim.x * ddxUVW.x;
    const float dvdx = dim.y * ddxUVW.y;
    const float dwdx = dim.z * ddxUVW.z;
    const float dudy = dim.x * ddyUVW.x;
    const float dvdy = dim.y * ddyUVW.y;
    const float dwdy = dim.z * ddyUVW.z;
    float lengthX = sqrt(dudx*dudx + dvdx*dvdx + dwdx*dwdx);
    float lengthY = sqrt(dudy*dudy + dvdy*dvdy + dwdy*dwdy);
    const float log2MaxAxis = log2(max(lengthX, lengthY));
    int ilod = STF_StochasticLinear(log2MaxAxis, u);
    return (uint)clamp(ilod, minLod, maxLod);
}


// If not using the sampler from d3d then the various filter variations need to be implemented in SW:

// Applies the texture addressing mode specified by 'mode' (one of the STF_ADDRESS_MODE_... constants)
// to the texture coordinate 'u' and returns the sample position inside the (0, 1) interval.
// The 'size' parameter is the texture size in the corresponding dimension on the right mip level.
// The 'isBorder' out parameter will be set to 'true' if the sample lands on a border in BORDER mode.
// Should be used in combination with one of the GetSamplePos functions above when no hardware sampler is available.
float STF_ApplyAddressingMode1D(float u,
                                uint size,
                                uint mode,
                                out bool isBorder)
{
    float invSize = 1.0 / float(size);
    isBorder = false;
    switch (mode)
    {
        case STF_ADDRESS_MODE_WRAP:
        default:
            return frac(u);

        case STF_ADDRESS_MODE_MIRROR:
            return 1.0 - abs(1.0 - frac(u * 0.5) * 2.0);

        case STF_ADDRESS_MODE_CLAMP:
            return clamp(u, invSize, 1.0 - invSize);

        case STF_ADDRESS_MODE_BORDER:
            isBorder = (u <= 0.0) || (u >= 1.0);
            return clamp(u, invSize, 1.0 - invSize);

        case STF_ADDRESS_MODE_MIRROR_ONCE:
            return clamp(abs(u), invSize, 1.0 - invSize);
    }
}

// 2D version of the ApplyAddressingMode function. See STF_ApplyAddressingMode1D for more info.
float2 STF_ApplyAddressingMode2D(float2 uv,
                                 uint2 size,
                                 uint2 modes,
                                 out bool isBorder)
{
    bool2 borders;
    float2 results;
    results.x = STF_ApplyAddressingMode1D(uv.x, size.x, modes.x, borders.x);
    results.y = STF_ApplyAddressingMode1D(uv.y, size.y, modes.y, borders.y);
    isBorder = any(borders);
    return results;
}

// 3D version of the ApplyAddressingMode function. See STF_ApplyAddressingMode1D for more info.
float3 STF_ApplyAddressingMode3D(float3 uvw,
                                 uint3 size,
                                 uint3 modes,
                                 out bool isBorder)
{
    bool3 borders;
    float3 results;
    results.x = STF_ApplyAddressingMode1D(uvw.x, size.x, modes.x, borders.x);
    results.y = STF_ApplyAddressingMode1D(uvw.y, size.y, modes.y, borders.y);
    results.z = STF_ApplyAddressingMode1D(uvw.z, size.z, modes.z, borders.z);
    isBorder = any(borders);
    return results;
}

struct STF_SamplerStateImpl
{
    uint _GetFilterType()
    {
        return m_filterType;
    }
    uint _GetAnisoMethod()
    {
        return m_anisoMethod;
    }
    uint _GetMagMethod()
    {
        return m_magMethod;
    } 
    uint3 _GetAddressingModes()
    {
        return m_addressingModes;
    }
    float _GetSigma()
    {
        return m_sigma;
    }
    uint4 _GetUserData()
    {
        return m_userData;
    }
    
    float _ComputeTextureLod(
                            uint2  dim,
                            float4 textureGrads,
                            float  minLod,
                            float  maxLod,
                            float  mipBias)
{
    float lod = 0;
    if (_GetAnisoMethod() == STF_ANISO_LOD_METHOD_DEFAULT)
    {
        lod = STF_ComputeTextureLod_STF(dim, textureGrads, minLod, maxLod, mipBias);
    }
    return lod;
}

STF_MUTATING 
float3 _GetTexture2DSamplePos(  uint mipValueType,
                                uint width,
                                uint height,
                                uint numberOfLevels,
                                float2 uv,
                                float2 ddxUV,
                                float2 ddyUV,
                                float mipValue)
{
    float4 u = m_u.xyzw;
    int maxLevel = (int)numberOfLevels - 1;

    // Stochastically compute the texture mip level
    uint lod = 0;
    if (mipValueType == STF_MIP_VALUE_MODE_NONE)
    {
        const float lodf = _ComputeTextureLod(uint2(width, height), float4(ddxUV, ddyUV), 0, maxLevel, 0.f);

        int ilod = STF_StochasticLinear(lodf, u.w);
        lod = (uint)clamp(ilod, 0, maxLevel);
    }
    else if (mipValueType == STF_MIP_VALUE_MODE_MIP_LEVEL)
    {
        int ilod = STF_StochasticLinear(mipValue, u.w);
        lod = (uint)clamp(ilod, 0, maxLevel);
    }
    else if (mipValueType == STF_MIP_VALUE_MODE_MIP_BIAS)
    {
        const float lodf = _ComputeTextureLod(uint2(width, height), float4(ddxUV, ddyUV), 0, maxLevel, mipValue);
        int ilod = STF_StochasticLinear(lodf, u.w);
        lod = (uint)clamp(ilod, 0, maxLevel);
    }

    // Query the W/H for the specified mip level.
    width = max(1u, width >> lod);
    height = max(1u, height >> lod);

    // Convert the uv coordinate to a texel position
    const float2 st = uv * float2(width, height) - 0.5f;
    int2 idx = 0;

    if (_GetFilterType() == STF_FILTER_TYPE_POINT)
    {
        idx = STF_NearestNeighbor2D(st);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_LINEAR)
    {
        idx = STF_StochasticBilinear(st, u.xy);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_CUBIC)
    {
        idx = STF_StochasticBicubic(st, u.x, u.y);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_GAUSSIAN)
    {
        idx = STF_StochasticGaussian2D(m_sigma, st, u.xy);
    }

    float2 idxUV = (idx + 0.5f) / float2(width, height);

    if (m_reseedOnSample)
    {
        m_u = u;
    }
        
    return float3(idxUV, lod);
}

STF_MUTATING
float4 _GetTextureCubeSamplePos(uint   mipValueType,
                                uint   width,
                                uint   numberOfLevels,
                                float3 uv,
                                float3 ddxUVW,
                                float3 ddyUVW,
                                float  mipValue)
{
    float4 u = m_u.xyzw;
    int maxLevel = (int)numberOfLevels - 1;

    // Stochastically compute the texture mip level
    uint lod = 0;
    if (mipValueType == STF_MIP_VALUE_MODE_NONE ||
        mipValueType == STF_MIP_VALUE_MODE_MIP_BIAS)
    {
        lod = uint(STF_ComputeIsotropicLod_Cube(uv, width, ddxUVW, ddyUVW, 0, maxLevel, mipValue, u.w));
    }
    else if (mipValueType == STF_MIP_VALUE_MODE_MIP_LEVEL)
    {
        int ilod = STF_StochasticLinear(mipValue, u.w);
        lod = (uint)clamp(ilod, 0, maxLevel);
    }

    // Calculate the width for the specified mip level.
    width = max(1u, width >> lod);

    // Compute the stochastic discrete UV coordinate.
    const float3 offset = float3(width, width, width) / 2.0f;
    const float3 normalizedUV = normalize(uv);
    const float3 st = normalizedUV * offset;
    int3 idx = 0;

    if (_GetFilterType() == STF_FILTER_TYPE_POINT)
    {
        idx = STF_NearestNeighbor3D(st);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_LINEAR)
    {
        idx = STF_StochasticTrilinear(st, u.xyz);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_CUBIC)
    {
        idx = STF_StochasticTricubic(st, u.xyz);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_GAUSSIAN)
    {
        idx = STF_StochasticGaussian3D(m_sigma, st, u.xyz);
    }

    float3 idxUV = idx / offset;

    if (m_reseedOnSample)
    {
        m_u = u;
    }

    return float4(idxUV, lod);
}

STF_MUTATING
float4 _GetTexture3DSamplePos(uint   mipValueType,
                                uint   width,
                                uint   height,
                                uint   depth,
                                uint   numberOfLevels,
                                float3 uv,
                                float3 ddxUVW,
                                float3 ddyUVW,
                                float  mipValue)
{
    float4 u = m_u.xyzw;
    int maxLevel = (int)numberOfLevels - 1;

    // Stochastically compute the texture mip level
    uint lod = 0;
    if (mipValueType == STF_MIP_VALUE_MODE_NONE ||
        mipValueType == STF_MIP_VALUE_MODE_MIP_BIAS)
    {
        lod = uint(STF_ComputeIsotropicLod_3D(uv, uint3(width, height, depth), ddxUVW, ddyUVW, 0, maxLevel, mipValue, u.w));
    }
    else if (mipValueType == STF_MIP_VALUE_MODE_MIP_LEVEL)
    {
        int ilod = STF_StochasticLinear(mipValue, u.w);
        lod = (uint)clamp(ilod, 0 /*minLod*/, maxLevel);
    }

    // Calculate the W/H/D for the specified mip level.
    width = max(1u, width >> lod);
    height = max(1u, height >> lod);
    depth = max(1u, depth >> lod);

    // Convert the uv coordinate to a texel position
    const float3 st = uv * float3(width, height, depth) - 0.5f;
    int3 idx = 0;

    if (_GetFilterType() == STF_FILTER_TYPE_POINT)
    {
        idx = STF_NearestNeighbor3D(st);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_LINEAR)
    {
        idx = STF_StochasticTrilinear(st, u.xyz);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_CUBIC)
    {
        idx = STF_StochasticTricubic(st, u.xyz);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_GAUSSIAN)
    {
        idx = STF_StochasticGaussian3D(m_sigma, st, u.xyz);
    }

    float3 idxUV = (idx + 0.5f) / float3(width, height, depth);

    if (m_reseedOnSample)
    {
        m_u = u;
    }

    return float4(idxUV, lod);
}

float _GetFilterPDF(float2 scaledUV, float2 scaledTexelPosition)
{
    float2 filterPDF;
    float2 texelDistance = scaledUV - scaledTexelPosition;
    if (_GetFilterType() == STF_FILTER_TYPE_LINEAR)
    {
        filterPDF = clamp(1 - abs(texelDistance), 0, 1);
    }
    else if (_GetFilterType() == STF_FILTER_TYPE_CUBIC)
    {
        filterPDF = float2(STF_BSplineCubic_PDF(texelDistance.x), STF_BSplineCubic_PDF(texelDistance.y));
    }
    else // if (GetFilterType() == STF_FILTER_TYPE_GAUSSIAN)
    {
        filterPDF = float2(STF_Gaussian_PDF(texelDistance.x, m_sigma), STF_Gaussian_PDF(texelDistance.y, m_sigma));
    }

    return filterPDF.x * filterPDF.y;
}

float _GetQuadShareWeight(float2 texelCoord, int2 coordOther, float otherPDF)
{
    // This is needed only in compute shaders and for... shadows.
    // Some lanes will be inactive, and then the returned value is 0.0f.
    // Normally, this should not happen.
    if (otherPDF == 0.0f)
        return 0.0f;

    return _GetFilterPDF(texelCoord, coordOther)/otherPDF;
}
    
#if STF_ALLOW_WAVE_READ
uint4 GetActiveThreadMask()
{
#if STF_SHADER_MODEL_MAJOR >= 6 && STF_SHADER_MODEL_MINOR >= 6 && (STF_SHADER_STAGE == STF_SHADER_STAGE_PIXEL)
    // WaveReadLaneAt is undefined when reading from helper lanes.
    const uint4 activeThreads = WaveActiveBallot( !IsHelperLane() );
#else
    const uint4 activeThreads = WaveActiveBallot(true);
#endif
    return activeThreads;
}

bool IsLaneActive(uint lane, uint4 activeThreadMask)
{
    const uint iMask = 1u << lane;
    return (activeThreadMask.x & iMask) == iMask;
}
#endif


float4 _Tex2D2x2WaveImpl(float4 val, float2 uv, float2 samplePos, uint width, uint height, uint method, uint frameNo)
{
#if STF_ALLOW_WAVE_READ
    // Setup for quad communication
    float2 texelCoord = float2(width, height) * uv - 0.5;
    int2 coordToSample = int2(round(float2(width, height) * samplePos.xy - 0.5));
    float samplePDF = _GetFilterPDF(texelCoord, coordToSample);

    const uint4 activeThreadMask = GetActiveThreadMask();

    uint baseIndex;
    if(method == STF_MAGNIFICATION_METHOD_2x2_QUAD)
        baseIndex = WaveGetLaneIndex() & 22;   // Make it even and remove bit 4th bit (with value 8).
    else if(method == STF_MAGNIFICATION_METHOD_2x2_FINE)
    {
        const uint l = WaveGetLaneIndex();
        uint allThreeLowestBitsSet = (((l >> 2) & (l >> 1) & l) & 1);
        uint bothBit3and4Set = ((l & (l >> 1)) & 8);
        uint offset = allThreeLowestBitsSet | bothBit3and4Set;
        baseIndex = l - offset;
    }
    else if (method == STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL)
    {
        const uint l = WaveGetLaneIndex();
        uint offset;
        if(bool(frameNo & 1)) // Odd frames.
        {
            uint allThreeLowestBitsSet = (((l >> 2) & (l >> 1) & l) & 1);
            uint bothBit3and4Set = ((l & (l >> 1)) & 8);
            offset = allThreeLowestBitsSet | bothBit3and4Set;
        }
        else // Even frames.
        {
            uint anyThreeLowestBitsSet = (((l >> 2) | (l >> 1) | l) & 1);
            uint eitherBit3and4Set = ((l | (l >> 1)) & 8);
            offset = anyThreeLowestBitsSet | eitherBit3and4Set;
        }
        baseIndex = l - offset;
    }
    float4 res = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float accum_w = 0.0f;
    for (uint i = 0; i < 4; i++)
    {
        uint laneID = baseIndex + (((i & 2) << 2) | (i & 1));   // Computes baseIndex + {0, 1, 8, 9}. That is, this can be unrolled easily by hand.
        if (IsLaneActive(laneID, activeThreadMask))
        {
            int2 coordOther = WaveReadLaneAt(coordToSample, laneID);
            float other_pdf = WaveReadLaneAt(samplePDF, laneID);
            float4 other_value = WaveReadLaneAt(val, laneID);
            float weight = _GetQuadShareWeight(texelCoord, coordOther, other_pdf);
            res += other_value * weight;
            accum_w += weight;
        }
    }

    return res * (1.0f / accum_w); // Might faster because division is done once and then multiplication, instead of possibly three divisions.
#else
        return 0.f;
#endif

}

uint GetBaseIndexWave3x3(uint l, bool useLUT)
{
    if (useLUT)
    {
        uint LUT[32] =
        {
            0, 0, 1, 2, 3, 4, 5, 5,
            0, 0, 1, 2, 3, 4, 5, 5,
            8, 8, 9, 10, 11, 12, 13, 13,
            8, 8, 9, 10, 11, 12, 13, 13
        };
            return LUT[l];
        }
    else
    {
        int t = l - ((((l >> 4) & 1) + ((l >> 3) & 1)) << 3);
        uint baseIndex2 = t < 8 ? min(max(t - 1, 0), 5) : min(max(t - 1, 8), 13);
        return baseIndex2;
    }
}

float4 _Tex2D3x3WaveImpl(float4 val, float2 uv, float2 samplePos, uint width, uint height, bool useLUT, uint frameNo)
{
#if STF_ALLOW_WAVE_READ
    const uint4 activeThreadMask = GetActiveThreadMask();

    // Setup for quad communication
    float2 texelCoord = float2(width, height) * uv - 0.5;
    int2 coordToSample = int2(round(float2(width, height) * samplePos.xy - 0.5));
    float samplePDF = _GetFilterPDF(texelCoord, coordToSample);
    float4 res = 0.0f;
    float accum_w = 0.0f;

    const uint l = WaveGetLaneIndex();
    uint baseIndex = GetBaseIndexWave3x3(l, useLUT);

    for (uint y = 0; y < 3; y++)
    {
        for (uint x = 0; x < 3; x++)
        {
            uint offset = (y << 3) | x;
            uint laneID = baseIndex + offset;
            if (IsLaneActive(laneID, activeThreadMask))
            {
                int2 coordOther = WaveReadLaneAt(coordToSample, laneID);
                float other_pdf = WaveReadLaneAt(samplePDF, laneID);
                float4 other_value = WaveReadLaneAt(val, laneID);
                float weight = _GetQuadShareWeight(texelCoord, coordOther, other_pdf);
                res += other_value * weight;
                accum_w += weight;
            }
        }
    }
    return res * (1.0f / accum_w);
#else
    return 0.f;
#endif
}

float4 _Tex2D4x4WaveImpl(float4 val, float2 uv, float2 samplePos, uint width, uint height, uint method, uint frameNo)
{
#if STF_ALLOW_WAVE_READ
    const uint4 activeThreadMask = GetActiveThreadMask();

    // Setup for quad communication
    float2 texelCoord = float2(width, height) * uv - 0.5;
    int2 coordToSample = int2(round(float2(width, height) * samplePos.xy - 0.5));
    float samplePDF = _GetFilterPDF(texelCoord, coordToSample);
    float4 res = 0.0f;
    float accum_w = 0.0f;

    const uint l = WaveGetLaneIndex();
    uint bit0 = l & 1;
    uint bit1 = (l >> 1) & 1;
    uint bit2 = (l >> 2) & 1;
    uint y2 = bit2 & bit1;
    uint y1 = (~bit2 & bit1 & bit0) | (bit2 & ~bit1);
    uint y0 = (~bit2 & bit1 & ~bit0) | (bit2 & ~bit1 & bit0);
    uint baseIndex = (y2 << 2) | (y1 << 1) | y0;
    for (uint i = 0; i < 16; i++)
    {
        uint offset = ((i << 1) & 24) | (i & 3);    // Becomes {0,1,2,3, 8,9,10,11, 16,17,18,19, 24,25,26,27}.
        uint laneID = baseIndex + offset;
        if (IsLaneActive(laneID, activeThreadMask))
        {
            int2 coordOther = WaveReadLaneAt(coordToSample, laneID);
            float other_pdf = WaveReadLaneAt(samplePDF, laneID);
            float4 other_value = WaveReadLaneAt(val, laneID);
            float weight = _GetQuadShareWeight(texelCoord, coordOther, other_pdf);
            res += other_value * weight;
            accum_w += weight;
        }
    }
    return res * (1.0f / accum_w);
#else
    return 0.f;
#endif
}

float4 _Tex2DWaveImpl(float4 val, float2 uv, float2 samplePos, uint width, uint height)
{
#if STF_ALLOW_WAVE_READ
    const uint4 activeThreadMask = GetActiveThreadMask();
    
    // Setup for quad communication
    float2 texelCoord = float2(width, height) * uv - 0.5;
    int2 coordToSample = int2(round(float2(width, height) * samplePos.xy - 0.5));
    float samplePDF = _GetFilterPDF(texelCoord, coordToSample);
    float4 res = 0.0f;
    float accum_w = 0.0f;
    uint base_index = (WaveGetLaneIndex() / STF_WAVE_READ_SAMPLES_PER_PIXEL) * STF_WAVE_READ_SAMPLES_PER_PIXEL;
    for (uint id0 = 0; id0 < STF_WAVE_READ_SAMPLES_PER_PIXEL; ++id0)
    {
        const uint i = base_index + id0;
        if (IsLaneActive(i, activeThreadMask))
        {
            int2 coordOther = WaveReadLaneAt(coordToSample, i);
            float other_pdf = WaveReadLaneAt(samplePDF, i);
            float4 other_value = WaveReadLaneAt(val, i);
            float weight = _GetQuadShareWeight(texelCoord, coordOther, other_pdf);
            res += other_value * weight;
            accum_w += weight;
        }
    }
    return res / accum_w;
#else
    return 0.f;
#endif
}
    
float4 _Texture2DMagImpl(float4 val, float2 uv, float2 samplePos, uint width, uint height)
{
#if STF_ALLOW_WAVE_READ
    if (STF_MAGNIFICATION_METHOD_2x2_QUAD == _GetMagMethod())
    {
        return _Tex2D2x2WaveImpl(val, uv, samplePos, width, height, STF_MAGNIFICATION_METHOD_2x2_QUAD, 0);
    }
    if (STF_MAGNIFICATION_METHOD_2x2_FINE == _GetMagMethod())
    {
        return _Tex2D2x2WaveImpl(val, uv, samplePos, width, height, STF_MAGNIFICATION_METHOD_2x2_FINE, 0);
    }
    if (STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL == _GetMagMethod())
    {
        return _Tex2D2x2WaveImpl(val, uv, samplePos, width, height, STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL, m_frameIndex);
    }
    if (STF_MAGNIFICATION_METHOD_3x3_FINE_LUT == _GetMagMethod())
    {
        return _Tex2D3x3WaveImpl(val, uv, samplePos, width, height, true /*useLUT*/, 0);
    }
    if (STF_MAGNIFICATION_METHOD_3x3_FINE_ALU == _GetMagMethod())
    {
        return _Tex2D3x3WaveImpl(val, uv, samplePos, width, height, false /*useLUT*/, 0);
    }
    if (STF_MAGNIFICATION_METHOD_4x4_FINE == _GetMagMethod())
    {
        return _Tex2D4x4WaveImpl(val, uv, samplePos, width, height, STF_MAGNIFICATION_METHOD_4x4_FINE, 0);
    }
#endif
    return val;
}

STF_MUTATING
float4 _Texture2DSampleImpl(
                            uint         mipValueType,
                            Texture2D    tex,
                            SamplerState s,
                            float2       uv,
                            float2       ddxUV,
                            float2       ddyUV,
                            float        mipValue)
{

    uint width;
    uint height;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, numberOfLevels);

    float3 samplePos = _GetTexture2DSamplePos(mipValueType, width, height, numberOfLevels, uv, ddxUV, ddyUV, mipValue);
    uint lod = uint(samplePos.z);
    width = width >> lod;
    height = height >> lod;

    if (_GetFilterType() == STF_FILTER_TYPE_POINT)
    {
        return tex.SampleLevel(s, samplePos.xy, samplePos.z);
    }

    // We use SampleLevel with the supplied sampler to make sure
    // we capture the right tiling mode (Wrap, Clamp and Mirror modes)
    const float4 val = tex.SampleLevel(s, samplePos.xy, samplePos.z);
    
    return _Texture2DMagImpl(val, uv, samplePos.xy, width, height);
}

STF_MUTATING
float4 _Texture2DLoadImpl(
                            uint mipValueType,
                            Texture2D tex,
                            float2 uv,
                            float2 ddxUV,
                            float2 ddyUV,
                            float mipValue)
{

    uint width;
    uint height;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, numberOfLevels);

    float3 samplePos = _GetTexture2DSamplePos(mipValueType, width, height, numberOfLevels, uv, ddxUV, ddyUV, mipValue);
    uint lod = uint(samplePos.z);
    width = width >> lod;
    height = height >> lod;

    bool isBorder = false;
    const float2 pixelIndex = uint2(width, height) * STF_ApplyAddressingMode2D(samplePos.xy, uint2(width, height), m_addressingModes.xy, isBorder);
    const float4 val = tex.Load(uint3((uint)pixelIndex.x, (uint)pixelIndex.y, lod));
    
    if (_GetFilterType() == STF_FILTER_TYPE_POINT)
    {
        return val;
    }

    return _Texture2DMagImpl(val, uv, samplePos.xy, width, height);
}
    
STF_MUTATING
float4 _Texture2DArraySampleImpl(uint           mipValueType,
                                    Texture2DArray tex,
                                    SamplerState   s,
                                    float3         uv,
                                    float3         ddxUV,
                                    float3         ddyUV,
                                    float          mipValue)
{
    uint width;
    uint height;
    uint TextureSlice = uint(uv.z);
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, TextureSlice, numberOfLevels);

    float3 samplePos = _GetTexture2DSamplePos(mipValueType, width, height, numberOfLevels, uv.xy, ddxUV.xy, ddyUV.xy, mipValue);

    // We use SampleLevel with the supplied sampler to make sure
    // we capture the right tiling mode (Wrap, Clamp and Mirror modes)
    return tex.SampleLevel(s, float3(samplePos.xy, uv.z), samplePos.z);
}

STF_MUTATING
float4 _Texture2DArrayLoadImpl(uint mipValueType,
                                    Texture2DArray tex,
                                    float3         uv,
                                    float3         ddxUV,
                                    float3         ddyUV,
                                    float          mipValue)
{
    uint width;
    uint height;
    uint numSlices;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, numSlices, numberOfLevels);

    float3 samplePos = _GetTexture2DSamplePos(mipValueType, width, height, numberOfLevels, uv.xy, ddxUV.xy, ddyUV.xy, mipValue);
    uint lod = uint(samplePos.z);
    width = width >> lod;
    height = height >> lod;

    bool isBorder = false;
    const float2 pixelIndex = uint2(width, height) * STF_ApplyAddressingMode2D(samplePos.xy, uint2(width, height), m_addressingModes.xy, isBorder);
    return tex.Load(uint4(uint(pixelIndex.x), uint(pixelIndex.y), (uint) uv.z, lod));
}

STF_MUTATING
float4 _TextureCubeSampleImpl(uint         mipValueType,
                                TextureCube  tex,
                                SamplerState s,
                                float3       uv,
                                float3       ddxUVW,
                                float3       ddyUVW,
                                float        mipValue)
{
    uint width;
    uint height;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, numberOfLevels);

    float4 samplePos = _GetTextureCubeSamplePos(mipValueType, width, numberOfLevels, uv, ddxUVW, ddyUVW, mipValue);

    // We use SampleLevel with the supplied sampler to make sure
    // we capture the right tiling mode (Wrap, Clamp and Mirror modes)
    return tex.SampleLevel(s, samplePos.xyz, samplePos.w);
}

STF_MUTATING
float4 _Texture3DSampleImpl(uint         mipValueType,
                            Texture3D    tex,
                            SamplerState s,
                            float3       uv,
                            float3       ddxUVW,
                            float3       ddyUVW,
                            float        mipValue)
{
    uint width;
    uint height;
    uint depth;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, depth, numberOfLevels);

    float4 samplePos = _GetTexture3DSamplePos(mipValueType, width, height, depth, numberOfLevels, uv, ddxUVW, ddyUVW, mipValue);

    // We use SampleLevel with the supplied sampler to make sure
    // we capture the right tiling mode (Wrap, Clamp and Mirror modes)
    return tex.SampleLevel(s, samplePos.xyz, samplePos.w);
}

STF_MUTATING
float4 _Texture3DLoadImpl(uint mipValueType,
                            Texture3D    tex,
                            float3       uv,
                            float3       ddxUVW,
                            float3       ddyUVW,
                            float        mipValue)
{
    uint width;
    uint height;
    uint depth;
    uint numberOfLevels;
    tex.GetDimensions(0, width, height, depth, numberOfLevels);

    float4 samplePos = _GetTexture3DSamplePos(mipValueType, width, height, depth, numberOfLevels, uv, ddxUVW, ddyUVW, mipValue);
    uint lod = uint(samplePos.w);
    width = width >> lod;
    height = height >> lod;
    depth = depth >> lod;

    bool isBorder = false;
    const float3 pixelIndex = uint3(width, height, depth) * STF_ApplyAddressingMode3D(samplePos.xyz, uint3(width, height, depth), m_addressingModes.xyz, isBorder);
    return tex.Load(uint4(uint(pixelIndex.x), uint(pixelIndex.y), uint(pixelIndex.z), lod));
}

STF_MUTATING
float4 _Texture2DSample(Texture2D tex, SamplerState s, float2 uv)
{
    return _Texture2DSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddx(uv), ddy(uv), 0.f);
}
    
STF_MUTATING
float4 _Texture2DSampleGrad(Texture2D tex, SamplerState s, float2 uv, float2 ddxUV, float2 ddyUV)
{
    return _Texture2DSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture2DSampleLevel(Texture2D tex, SamplerState s, float2 uv, float mipLevel)
{
    return _Texture2DSampleImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, s, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture2DSampleBias(Texture2D tex, SamplerState s, float2 uv, float mipBias)
{
    return _Texture2DSampleImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, s, uv, 0, 0, mipBias);
}

STF_MUTATING
float4 _Texture2DLoad(Texture2D tex, float2 uv)
{
    return _Texture2DLoadImpl(STF_MIP_VALUE_MODE_NONE, tex,uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture2DLoadGrad(Texture2D tex, float2 uv, float2 ddxUV, float2 ddyUV)
{
    return _Texture2DLoadImpl(STF_MIP_VALUE_MODE_NONE, tex, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture2DLoadLevel(Texture2D tex, float2 uv, float mipLevel)
{
    return _Texture2DLoadImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture2DLoadBias(Texture2D tex, float2 uv, float mipBias)
{
    return _Texture2DLoadImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, uv, 0, 0, mipBias);
}

// Texture2D/Texture2DArray without the Texture objects.
// These functions return float3(x, y, lod) where (x, y) point at texel centers in UV space, lod is integer.
// Note: use floor(f) to convert the sample positions to integer texel coordinates, not round(f).

STF_MUTATING
float3 _Texture2DGetSamplePos(uint   width,
                                uint   height,
                                uint   numberOfLevels,
                                float2 uv)
{
    return _GetTexture2DSamplePos(STF_MIP_VALUE_MODE_NONE, width, height, numberOfLevels, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float3 _Texture2DGetSamplePosGrad(uint   width,
                                    uint   height,
                                    uint   numberOfLevels,
                                    float2 uv,
                                    float2 ddxUV,
                                    float2 ddyUV)
{
    return _GetTexture2DSamplePos(STF_MIP_VALUE_MODE_NONE, width, height, numberOfLevels, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float3 _Texture2DGetSamplePosLevel(uint   width,
                                    uint   height,
                                    uint   numberOfLevels,
                                    float2 uv,
                                    float  mipLevel)
{
    return _GetTexture2DSamplePos(STF_MIP_VALUE_MODE_MIP_LEVEL, width, height, numberOfLevels, uv, 0, 0, mipLevel);
}

STF_MUTATING
float3 _Texture2DGetSamplePosBias(uint   width,
                                    uint   height,
                                    uint   numberOfLevels,
                                    float2 uv,
                                    float  mipBias)
{
    return _GetTexture2DSamplePos(STF_MIP_VALUE_MODE_MIP_BIAS, width, height, numberOfLevels, uv, ddx(uv), ddy(uv), mipBias);
}

// Texture2DArray with the Texture objects.

STF_MUTATING
float4 _Texture2DArraySample(Texture2DArray tex,
                            SamplerState s,
                            float3 uv)
{
    return _Texture2DArraySampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture2DArraySampleGrad(Texture2DArray tex,
                                SamplerState   s,
                                float3         uv,
                                float3         ddxUV,
                                float3         ddyUV)
{
    return _Texture2DArraySampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture2DArraySampleLevel(Texture2DArray tex,
                                    SamplerState   s,
                                    float3         uv,
                                    float          mipLevel)
{
    return _Texture2DArraySampleImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, s, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture2DArraySampleBias(Texture2DArray tex,
                                SamplerState   s,
                                float3         uv,
                                float          mipBias)
{
    return _Texture2DArraySampleImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, s, uv, 0, 0, mipBias);
}

STF_MUTATING
float4 _Texture2DArrayLoad(Texture2DArray tex, float3 uv)
{
    return _Texture2DArrayLoadImpl(STF_MIP_VALUE_MODE_NONE, tex, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture2DArrayLoadGrad(Texture2DArray tex,
                                float3         uv,
                                float3         ddxUV,
                                float3         ddyUV)
{
    return _Texture2DArrayLoadImpl(STF_MIP_VALUE_MODE_NONE, tex, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture2DArrayLoadLevel(Texture2DArray tex,
                                    float3         uv,
                                    float          mipLevel)
{
    return _Texture2DArrayLoadImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture2DArrayLoadBias(Texture2DArray tex,
                                float3         uv,
                                float          mipBias)
{
    return _Texture2DArrayLoadImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, uv, 0, 0, mipBias);
}

// Texture2DArray without the Texture objects.  TODO

// Texture3D with the Texture objects.

STF_MUTATING
float4 _Texture3DSample(Texture3D    tex,
                        SamplerState s,
                        float3       uv)
{
    return _Texture3DSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture3DSampleGrad(Texture3D    tex,
                            SamplerState s,
                            float3       uv,
                            float3       ddxUV,
                            float3       ddyUV)
{
    return _Texture3DSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture3DSampleLevel(Texture3D    tex,
                            SamplerState s,
                            float3       uv,
                            float        mipLevel)
{
    return _Texture3DSampleImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, s, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture3DSampleBias(Texture3D    tex,
                            SamplerState s,
                            float3       uv,
                            float        mipBias)
{
    return _Texture3DSampleImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, s, uv, 0, 0, mipBias);
}

STF_MUTATING
float4 _Texture3DLoad(Texture3D tex, float3 uv)
{
    return _Texture3DLoadImpl(STF_MIP_VALUE_MODE_NONE, tex, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture3DLoadGrad(Texture3D tex, float3 uv, float3 ddxUV, float3 ddyUV)
{
    return _Texture3DLoadImpl(STF_MIP_VALUE_MODE_NONE, tex, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture3DLoadLevel(Texture3D tex, float3 uv, float mipLevel)
{
    return _Texture3DLoadImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture3DLoadBias(Texture3D tex, float3 uv, float mipBias)
{
    return _Texture3DLoadImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, uv, 0, 0, mipBias);
}

// Texture3D without the Texture objects.
// These functions return float4(x, y, z, lod) where (x, y, z) point at texel centers in UV space, lod is integer.
STF_MUTATING
float4 _Texture3DGetSamplePos(uint   width,
                                uint   height,
                                uint   depth,
                                uint   numberOfLevels,
                                float3 uv)
{
    return _GetTexture3DSamplePos(STF_MIP_VALUE_MODE_NONE, width, height, depth, numberOfLevels, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _Texture3DGetSamplePosGrad(uint   width,
                                    uint   height,
                                    uint   depth,
                                    uint   numberOfLevels,
                                    float3 uv,
                                    float3 ddxUV,
                                    float3 ddyUV)
{
    return _GetTexture3DSamplePos(STF_MIP_VALUE_MODE_NONE, width, height, depth, numberOfLevels, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _Texture3DGetSamplePosLevel(uint   width,
                                    uint   height,
                                    uint   depth,
                                    uint   numberOfLevels,
                                    float3 uv,
                                    float  mipLevel)
{
    return _GetTexture3DSamplePos(STF_MIP_VALUE_MODE_MIP_LEVEL, width, height, depth, numberOfLevels, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _Texture3DGetSamplePosBias(uint   width,
                                    uint   height,
                                    uint   depth,
                                    uint   numberOfLevels,
                                    float3 uv,
                                    float  mipBias)
{
    return _GetTexture3DSamplePos(STF_MIP_VALUE_MODE_MIP_BIAS, width, height, depth, numberOfLevels, uv, 0, 0, mipBias);
}

// TextureCube with the Texture objects.
STF_MUTATING
float4 _TextureCubeSample(TextureCube  tex,
                            SamplerState s,
                            float3       uv)
{
    return _TextureCubeSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddx(uv), ddy(uv), 0.f);
}

STF_MUTATING
float4 _TextureCubeSampleGrad(TextureCube  tex,
                                SamplerState s,
                                float3       uv,
                                float3       ddxUV,
                                float3       ddyUV)
{
    return _TextureCubeSampleImpl(STF_MIP_VALUE_MODE_NONE, tex, s, uv, ddxUV, ddyUV, 0.f);
}

STF_MUTATING
float4 _TextureCubeSampleLevel(TextureCube  tex,
                                SamplerState s,
                                float3       uv,
                                float        mipLevel)
{
    return _TextureCubeSampleImpl(STF_MIP_VALUE_MODE_MIP_LEVEL, tex, s, uv, 0, 0, mipLevel);
}

STF_MUTATING
float4 _TextureCubeSampleBias(TextureCube  tex,
                                SamplerState s,
                                float3       uv,
                                float        mipBias)
{
    return _TextureCubeSampleImpl(STF_MIP_VALUE_MODE_MIP_BIAS, tex, s, uv, 0, 0, mipBias);
}

static STF_SamplerStateImpl _Create(float4 u)
{
    STF_SamplerStateImpl p;
    p.m_filterType       = STF_FILTER_TYPE_LINEAR;
    p.m_frameIndex       = 0;
    p.m_anisoMethod      = STF_ANISO_LOD_METHOD_DEFAULT;
    p.m_magMethod        = STF_MAGNIFICATION_METHOD_2x2_QUAD;
    p.m_addressingModes  = uint3(STF_ADDRESS_MODE_WRAP, STF_ADDRESS_MODE_WRAP, STF_ADDRESS_MODE_WRAP);
    p.m_sigma            = 0.7;
    p.m_u                = u;
    p.m_userData         = 0;
    return p;
}

uint  m_filterType;      // STF_FILTER_TYPE_*
uint  m_frameIndex;
uint  m_anisoMethod;     // STF_ANISO_LOD_METHOD_*
uint  m_magMethod;       // STF_MAGNIFICATION_METHOD_*
uint3 m_addressingModes; // STF_ADDRESS_MODE_*
float m_sigma;           // used for Gaussian kernel
float4 m_u;              // uniform random number(s)
bool m_reseedOnSample;
uint4 m_userData;        // User data, can be used to store some application specific auxilary data in the sampler object

};

#endif // #ifndef __STF_SAMPLER_STATE_IMPL_HLSLI__