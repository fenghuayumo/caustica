#pragma pack_matrix(row_major)

#include <shaders/tonemapping_cb.h>

Buffer<uint> t_Histogram : register(t0);
RWBuffer<uint> u_Exposure : register(u0);

cbuffer c_ToneMapping : register(b0)
{
    ToneMappingConstants g_ToneMapping;
};

#define FIXED_POINT_FRAC_BITS 6
#define FIXED_POINT_FRAC_MULTIPLIER (1 << FIXED_POINT_FRAC_BITS)

[numthreads(1, 1, 1)]
void main()
{
    float cdf = 0;
    uint i;

    [loop]
    for (i = 0; i < HISTOGRAM_BINS; ++i)
    {
        cdf += float(t_Histogram[i]) / FIXED_POINT_FRAC_MULTIPLIER;
    }

    float lowCdf = cdf * g_ToneMapping.histogramLowPercentile;
    float highCdf = cdf * g_ToneMapping.histogramHighPercentile;

    float weightSum = 0;
    float binSum = 0;
    cdf = 0;

    [loop]
    for (i = 0; i < HISTOGRAM_BINS; ++i)
    {
        float binValue = float(t_Histogram[i]) / FIXED_POINT_FRAC_MULTIPLIER;

        if (lowCdf <= cdf + binValue && cdf <= highCdf)
        {
            float histogramBinLuminance = exp2((i / (float)HISTOGRAM_BINS) * g_ToneMapping.logLuminanceScale
                + g_ToneMapping.logLuminanceBias);

            weightSum += histogramBinLuminance * binValue;
            binSum += binValue;
        }

        cdf += binValue;
    }

    float targetExposure = (binSum > 0) ? (weightSum / binSum) : 0;
    
    targetExposure = clamp(
        targetExposure,
        g_ToneMapping.minAdaptedLuminance, 
        g_ToneMapping.maxAdaptedLuminance);
    
    float oldExposure = asfloat(u_Exposure[0]);
    float diff = oldExposure - targetExposure;

    float adaptationSpeed = (diff < 0)
        ? g_ToneMapping.eyeAdaptationSpeedUp
        : g_ToneMapping.eyeAdaptationSpeedDown;

    if (adaptationSpeed > 0)
    {
        targetExposure += diff * exp2(-g_ToneMapping.frameTime * adaptationSpeed);
    }

    u_Exposure[0] = asuint(targetExposure);
}
