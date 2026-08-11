#ifndef __CAUSTICA_STOCHASTIC_TEXTURE_SAMPLER_HLSLI__
#define __CAUSTICA_STOCHASTIC_TEXTURE_SAMPLER_HLSLI__

// Caustica's compact stochastic sampler. The numeric values are part of the
// PathTracerConstants ABI shared with PathTracePass.cpp.
#define CAUSTICA_STF_FILTER_POINT       0
#define CAUSTICA_STF_FILTER_LINEAR      1
#define CAUSTICA_STF_FILTER_CUBIC       2
#define CAUSTICA_STF_FILTER_GAUSSIAN    3

#define CAUSTICA_STF_MAG_NONE           0
#define CAUSTICA_STF_MAG_QUAD_2X2       1
#define CAUSTICA_STF_MAG_FINE_2X2       2
#define CAUSTICA_STF_MAG_TEMPORAL_2X2   3
#define CAUSTICA_STF_MAG_FINE_ALU_3X3   4
#define CAUSTICA_STF_MAG_FINE_LUT_3X3   5
#define CAUSTICA_STF_MAG_FINE_4X4       6

struct CausticaStochasticTextureSampler
{
    float4 random;
    uint frameIndex;
    uint filterMode;
    uint magnificationMethod;
    float gaussianSigma;

    static CausticaStochasticTextureSampler Create(float4 uniformRandom)
    {
        CausticaStochasticTextureSampler result;
        result.random = frac(uniformRandom);
        result.frameIndex = 0;
        result.filterMode = CAUSTICA_STF_FILTER_LINEAR;
        result.magnificationMethod = CAUSTICA_STF_MAG_QUAD_2X2;
        result.gaussianSigma = 0.7f;
        return result;
    }

    void SetFrameIndex(uint value) { frameIndex = value; }
    void SetFilterType(uint value) { filterMode = value; }
    void SetMagMethod(uint value) { magnificationMethod = value; }
    void SetSigma(float value) { gaussianSigma = max(value, 1.0e-3f); }

    float2 sampleJitter(float2 uv)
    {
        float2 seed = frac(random.xy + uv * float2(0.754877666f, 0.569840296f));
        if (magnificationMethod == CAUSTICA_STF_MAG_TEMPORAL_2X2)
            seed = frac(seed + float2(frameIndex & 1u, (frameIndex >> 1u) & 1u) * 0.5f);

        if (filterMode == CAUSTICA_STF_FILTER_GAUSSIAN)
        {
            float radius = sqrt(-2.0f * log(max(seed.x, 1.0e-6f))) * gaussianSigma;
            float angle = 6.28318530718f * seed.y;
            return radius * float2(cos(angle), sin(angle));
        }

        float scale = 1.0f;
        if (filterMode == CAUSTICA_STF_FILTER_CUBIC)
            scale = 1.5f;
        if (magnificationMethod == CAUSTICA_STF_MAG_FINE_ALU_3X3 ||
            magnificationMethod == CAUSTICA_STF_MAG_FINE_LUT_3X3)
            scale *= 1.5f;
        else if (magnificationMethod == CAUSTICA_STF_MAG_FINE_4X4)
            scale *= 2.0f;

        return (seed - 0.5f) * scale;
    }

    float4 Texture2DSampleLevel(Texture2D textureObject, SamplerState samplerObject, float2 uv, float lod)
    {
        if (filterMode == CAUSTICA_STF_FILTER_POINT || magnificationMethod == CAUSTICA_STF_MAG_NONE)
            return textureObject.SampleLevel(samplerObject, uv, lod);

        uint width;
        uint height;
        uint mipCount;
        textureObject.GetDimensions(0, width, height, mipCount);

        float clampedLod = clamp(lod, 0.0f, max(float(mipCount) - 1.0f, 0.0f));
        float2 mipDimensions = max(float2(width, height) * exp2(-floor(clampedLod)), 1.0f.xx);
        float2 stochasticUv = uv + sampleJitter(uv) / mipDimensions;
        return textureObject.SampleLevel(samplerObject, stochasticUv, clampedLod);
    }
};

#endif // __CAUSTICA_STOCHASTIC_TEXTURE_SAMPLER_HLSLI__
