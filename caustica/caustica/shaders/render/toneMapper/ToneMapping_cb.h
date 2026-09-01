#ifndef TONEMAPPING_CB_H
#define TONEMAPPING_CB_H

#define TONEMAPPING_AUTOEXPOSURE_CPU     1
#define TONEMAPPING_EXPOSURE_KEY         0.042

enum class ToneMapperOperator : uint32_t
{
    Linear,                 ///< Linear mapping
    Reinhard,               ///< Reinhard operator
    ReinhardModified,       ///< Reinhard operator with maximum white intensity
    HejiHableAlu,           ///< John Hable's ALU approximation of Jim Heji's filmic operator
    HableUc2,               ///< John Hable's filmic tone-mapping used in Uncharted 2
    Aces,                   ///< Aces Filmic Tone-Mapping
    PbrNeutral,             ///< Khronos PBR Neutral (midtones near-linear, highlight compression)
    IdentitySoftShoulder,   ///< Identity below a threshold, soft highlight shoulder
    AgX,                    ///< AgX (cinematic, wide-gamut highlight rolloff)
    CameraLut,              ///< Camera 1D LUT without an additional tone curve
};


struct ToneMappingConstants
{
    float whiteScale;
    float whiteMaxLuminance;
    uint toneMapOperator;
    uint clamped;
    uint autoExposure;
    float avgLuminance;
    float autoExposureLumValueMin;
    float autoExposureLumValueMax;
    float3x4 colorTransform;
    uint enabled;
    uint _padding0;
    uint _padding1;
    uint _padding2;
    uint cameraLutEnabled;
    float3 cameraLutDomainMin;
    float3 cameraLutDomainMax;
    uint cameraLutSize;
    uint cameraLutAfterToneMap;
    uint cameraLutIs3D;
    uint _cameraLutPadding1;
    uint _cameraLutPadding2;
    float4 cameraLut[256];
};


#endif // TONEMAPPING_CB_H
