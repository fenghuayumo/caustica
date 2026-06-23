#ifndef SURFACE_HLSLI
#define SURFACE_HLSLI

struct MaterialSubsurfaceParams
{
    float3 transmissionColor;
    float3 scatteringColor;
    float scale;
    float anisotropy;
};

struct MaterialHairParams
{
    float3 baseColor;
    float melanin;
    float melaninRedness;
    float longitudinalRoughness;
    float azimuthalRoughness;
    float ior;
    float cuticleAngle;
    float3 diffuseReflectionTint;
    float diffuseReflectionWeight;
};

struct MaterialSample
{
    float3 shadingNormal;
    float3 geometryNormal;
    float3 diffuseAlbedo; // BRDF input Cdiff
    float3 specularF0; // BRDF input F0
    float3 emissiveColor;
    float opacity;
    float occlusion;
    float roughness;
    float3 baseColor; // native in metal-rough, derived in spec-gloss
    float metalness; // native in metal-rough, derived in spec-gloss
    float transmission;
    bool hasMetalRoughParams; // indicates that 'baseColor' and 'metalness' are valid
    MaterialSubsurfaceParams subsurfaceParams;
    MaterialHairParams hairParams;
};

MaterialSample DefaultMaterialSample()
{
    MaterialSample result;
    result.shadingNormal = 0;
    result.geometryNormal = 0;
    result.diffuseAlbedo = 0;
    result.specularF0 = 0;
    result.emissiveColor = 0;
    result.opacity = 1;
    result.occlusion = 1;
    result.roughness = 0;
    result.baseColor = 0;
    result.metalness = 0;
    result.transmission = 0;
    result.hasMetalRoughParams = false;
    result.subsurfaceParams.transmissionColor = 0;
    result.subsurfaceParams.scatteringColor = 0;
    result.subsurfaceParams.scale = 0;
    result.subsurfaceParams.anisotropy = 0;
    result.hairParams.baseColor = 0;
    result.hairParams.melanin = 0;
    result.hairParams.melaninRedness = 0;
    result.hairParams.longitudinalRoughness = 0;
    result.hairParams.azimuthalRoughness = 0;
    result.hairParams.ior = 0;
    result.hairParams.cuticleAngle = 0;
    result.hairParams.diffuseReflectionTint = 0;
    result.hairParams.diffuseReflectionWeight = 0;
    return result;
}

#endif // SURFACE_HLSLI
