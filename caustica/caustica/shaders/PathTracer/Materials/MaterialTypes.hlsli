#ifndef __MATERIAL_TYPES_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __MATERIAL_TYPES_HLSLI__

#include "../Config.h"

// rename to MaterialPropertiesAtSurface?
struct MaterialProperties
{
    float3 shadingNormal;
    float3 geometryNormal;
    lpfloat3 diffuseAlbedo; // BRDF input Cdiff
    lpfloat3 specularF0; // BRDF input F0
    lpfloat3 emissiveColor;
    lpfloat opacity;
    //lpfloat occlusion;
    lpfloat roughness;
    lpfloat baseWeight;
    lpfloat specularWeight;
    lpfloat anisotropy;
    lpfloat fuzzWeight;
    lpfloat3 fuzzColor;
    lpfloat fuzzRoughness;
    lpfloat3 baseColor; // native in metal-rough, derived in spec-gloss
    lpfloat metalness; // native in metal-rough, derived in spec-gloss
#if !defined(CAUSTICA_MATERIAL_HAS_TRANSMISSION) || CAUSTICA_MATERIAL_HAS_TRANSMISSION
    lpfloat transmission;
    lpfloat diffuseTransmission;
#endif
    lpfloat ior;
    lpfloat shadowNoLFadeout;
    lpfloat unlitShadowStrength;
    uint flags;

    // OpenPBR coat / subsurface / thin-film / transmission extras
    lpfloat coatWeight;
    lpfloat3 coatColor;
    lpfloat coatRoughness;
    lpfloat coatAnisotropy;
    lpfloat coatIor;
    lpfloat coatDarkening;

    lpfloat subsurfaceWeight;
    lpfloat3 subsurfaceColor;
    lpfloat subsurfaceRadius;
    lpfloat subsurfaceScale;
    lpfloat subsurfaceAnisotropy;

    lpfloat thinFilmWeight;
    lpfloat thinFilmThickness;
    lpfloat thinFilmIor;

    lpfloat3 transmissionColor;
    lpfloat transmissionDepth;
    lpfloat3 transmissionScatter;
    lpfloat transmissionScatterAnisotropy;
    lpfloat transmissionDispersionScale;
    lpfloat transmissionDispersionAbbeNumber;

    static MaterialProperties make()
    {
        MaterialProperties result;
        result.shadingNormal = 0;
        result.geometryNormal = 0;
        result.diffuseAlbedo = 0;
        result.specularF0 = 0;
        result.emissiveColor = 0;
        result.opacity = 1;
        //result.occlusion = 1;
        result.roughness = 0;
        result.baseWeight = 1;
        result.specularWeight = 1;
        result.anisotropy = 0;
        result.fuzzWeight = 0;
        result.fuzzColor = 1;
        result.fuzzRoughness = 0.6;
        result.baseColor = 0;
        result.metalness = 0;
#if !defined(CAUSTICA_MATERIAL_HAS_TRANSMISSION) || CAUSTICA_MATERIAL_HAS_TRANSMISSION
        result.transmission = 0;
        result.diffuseTransmission = 0;
#endif
        result.ior = 1.5;
        result.flags = 0;
        result.shadowNoLFadeout = 0;
        result.unlitShadowStrength = 1;

        result.coatWeight = 0;
        result.coatColor = 1;
        result.coatRoughness = 0;
        result.coatAnisotropy = 0;
        result.coatIor = 1.6;
        result.coatDarkening = 1;

        result.subsurfaceWeight = 0;
        result.subsurfaceColor = 1;
        result.subsurfaceRadius = 1;
        result.subsurfaceScale = 1;
        result.subsurfaceAnisotropy = 0;

        result.thinFilmWeight = 0;
        result.thinFilmThickness = 0.5;
        result.thinFilmIor = 1.4;

        result.transmissionColor = 1;
        result.transmissionDepth = 0;
        result.transmissionScatter = 0;
        result.transmissionScatterAnisotropy = 0;
        result.transmissionDispersionScale = 0;
        result.transmissionDispersionAbbeNumber = 20;
        return result;
    }
};

#endif // __MATERIAL_TYPES_HLSLI__
