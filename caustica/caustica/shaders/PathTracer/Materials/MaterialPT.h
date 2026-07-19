#ifndef __PATH_TRACER_MATERIAL_H__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __PATH_TRACER_MATERIAL_H__

/// Max number of materials - could be dynamic but isn't for simplicity
#define CAUSTICA_MATERIAL_MAX_COUNT        32768

// using https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume#attenuation convention
struct VolumePTConstants
{
    float3  AttenuationColor;
    float   AttenuationDistance;
};

static const int PTMaterialFlags_UseSpecularGlossModel          = 0x00000001;
//static const int PTMaterialFlags_DoubleSided                    = 0x00000002;
static const int PTMaterialFlags_UseMetalRoughOrSpecularTexture = 0x00000004;
static const int PTMaterialFlags_UseBaseOrDiffuseTexture        = 0x00000008;
static const int PTMaterialFlags_UseEmissiveTexture             = 0x00000010;
static const int PTMaterialFlags_UseNormalTexture               = 0x00000020;
//static const int PTMaterialFlags_UseOcclusionTexture            = 0x00000040;
static const int PTMaterialFlags_UseTransmissionTexture         = 0x00000080;
static const int PTMaterialFlags_MetalnessInRedChannel          = 0x00000100;
static const int PTMaterialFlags_ThinSurface                    = 0x00000200;
static const int PTMaterialFlags_PSDExclude                     = 0x00000400;
static const int PTMaterialFlags_EnableAsAnalyticLightProxy     = 0x00000800;
static const int PTMaterialFlags_IgnoreMeshTangentSpace         = (1 << 12);
static const int PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB0     = (1 << 13);
static const int PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB1     = (1 << 14);
static const int PTMaterialFlags_UseOpenPBRMaterialModel        = (1 << 15);
static const int PTMaterialFlags_NestedPriorityMask             = 0xF0000000;
static const int PTMaterialFlags_NestedPriorityShift            = 28;
static const int PTMaterialFlags_PSDDominantDeltaLobeP1Mask     = 0x0F000000;
static const int PTMaterialFlags_PSDDominantDeltaLobeP1Shift    = 24;

/// Data with the packed layout in GPU memory
struct PTMaterialData
{
    float3      BaseOrDiffuseColor;
    uint        Flags;

    float3      SpecularColor;
    float       CoatWeight;

    float3      EmissiveColor;
    float       ShadowNoLFadeout;

    float       Opacity;
    float       Roughness;
    float       Metalness;
    float       NormalTextureScale;

    float       CoatRoughness;
    float       AlphaCutoff;
    float       TransmissionFactor;
    uint        BaseOrDiffuseTextureIndex;

    uint        MetalRoughOrSpecularTextureIndex;
    uint        EmissiveTextureIndex;
    uint        NormalTextureIndex;
    uint        OcclusionTextureIndex;

    uint        TransmissionTextureIndex;
    float       IoR;
    float       CoatIor;
    float       DiffuseTransmissionFactor;

    float       BaseWeight;
    float       SpecularWeight;
    float       Anisotropy;
    float       FuzzWeight;

    float3      FuzzColor;
    float       FuzzRoughness;

    float3      CoatColor;
    float       CoatDarkening;

    float       CoatAnisotropy;
    float       SubsurfaceWeight;
    float       SubsurfaceRadius;
    float       SubsurfaceScale;

    float3      SubsurfaceColor;
    float       SubsurfaceAnisotropy;

    float       ThinFilmWeight;
    float       ThinFilmThickness;
    float       ThinFilmIor;
    float       TransmissionDepth;

    float3      TransmissionColor;
    float       TransmissionDispersionScale;

    float3      TransmissionScatter;
    float       TransmissionDispersionAbbeNumber;

    float       TransmissionScatterAnisotropy;
    float       _padOpenPBR0;
    float       _padOpenPBR1;
    float       _padOpenPBR2;

    VolumePTConstants Volume;
};

#if defined(__cplusplus)
#endif

#endif
