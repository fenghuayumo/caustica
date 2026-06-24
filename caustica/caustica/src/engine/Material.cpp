#include <scene/SceneTypes.h>

using namespace caustica::math;
#include <shaders/material_cb.h>

namespace caustica
{
    void Material::FillConstantBuffer(MaterialConstants& constants, bool useResourceDescriptorHeapBindless) const
    {
        // Create lambda to handle bindless texture index retrieval
        auto GetBindlessTextureIndex = [useResourceDescriptorHeapBindless](const std::shared_ptr<LoadedTexture>& texture) -> int {
            if (!texture)
                return -1;
            
            return useResourceDescriptorHeapBindless 
                ? texture->bindlessDescriptor.GetIndexInHeap()
                : texture->bindlessDescriptor.Get();
        };

        // flags

        constants.flags = 0;

        if (useSpecularGlossModel)
            constants.flags |= MaterialFlags_UseSpecularGlossModel;

        if (baseOrDiffuseTexture && enableBaseOrDiffuseTexture)
            constants.flags |= MaterialFlags_UseBaseOrDiffuseTexture;

        if (metalRoughOrSpecularTexture && enableMetalRoughOrSpecularTexture)
            constants.flags |= MaterialFlags_UseMetalRoughOrSpecularTexture;

        if (emissiveTexture && enableEmissiveTexture)
            constants.flags |= MaterialFlags_UseEmissiveTexture;

        if (normalTexture && enableNormalTexture)
            constants.flags |= MaterialFlags_UseNormalTexture;

        if (occlusionTexture && enableOcclusionTexture)
            constants.flags |= MaterialFlags_UseOcclusionTexture;

        if (transmissionTexture && enableTransmissionTexture)
            constants.flags |= MaterialFlags_UseTransmissionTexture;

        if (opacityTexture && enableOpacityTexture)
            constants.flags |= MaterialFlags_UseOpacityTexture;

        if (doubleSided)
            constants.flags |= MaterialFlags_DoubleSided;

        if (metalnessInRedChannel)
            constants.flags |= MaterialFlags_MetalnessInRedChannel;

        // free parameters

        constants.domain = (int)domain;
        constants.baseOrDiffuseColor = baseOrDiffuseColor;
        constants.specularColor = specularColor;
        constants.emissiveColor = emissiveColor * emissiveIntensity;
        constants.roughness = roughness;
        constants.metalness = metalness;
        constants.normalTextureScale = normalTextureScale;
        constants.materialID = materialID;
        constants.occlusionStrength = occlusionStrength;
        constants.transmissionFactor = transmissionFactor;
        constants.normalTextureTransformScale = normalTextureTransformScale;
        
        switch (domain)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case MaterialDomain::AlphaBlended:
        case MaterialDomain::TransmissiveAlphaBlended:
            constants.opacity = opacity;
            break;

        case MaterialDomain::Opaque:
        case MaterialDomain::AlphaTested:
        case MaterialDomain::Transmissive:
        case MaterialDomain::TransmissiveAlphaTested:
            constants.opacity = 1.f;
            break;
        default:
            break;
        }
        
        switch(domain)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case MaterialDomain::AlphaTested:
        case MaterialDomain::TransmissiveAlphaTested:
            constants.alphaCutoff = alphaCutoff;
            break;

        case MaterialDomain::AlphaBlended:
        case MaterialDomain::TransmissiveAlphaBlended:
            constants.alphaCutoff = 0.f; // discard only if opacity == 0

        case MaterialDomain::Opaque:
        case MaterialDomain::Transmissive:
            constants.alphaCutoff = -1.f; // never discard
            break;
        default:
            break;
        }

        if (enableSubsurfaceScattering)
        {
            constants.flags |= MaterialFlags_SubsurfaceScattering;

            constants.sssTransmissionColor = subsurface.transmissionColor;
            constants.sssScatteringColor = subsurface.scatteringColor;
            constants.sssScale = subsurface.scale;
            constants.sssAnisotropy = subsurface.anisotropy;
        }

        if (enableHair)
        {
            constants.flags |= MaterialFlags_Hair;

            constants.hairBaseColor = hair.baseColor;
            constants.hairMelanin = hair.melanin;
            constants.hairMelaninRedness = hair.melaninRedness;
            constants.hairLongitudinalRoughness = hair.longitudinalRoughness;
            constants.hairAzimuthalRoughness = hair.azimuthalRoughness;
            constants.hairIor = hair.ior;
            constants.hairCuticleAngle = hair.cuticleAngle;
            constants.hairDiffuseReflectionWeight = hair.diffuseReflectionWeight;
            constants.hairDiffuseReflectionTint = hair.diffuseReflectionTint;
        }

        // bindless textures

        constants.baseOrDiffuseTextureIndex = GetBindlessTextureIndex(baseOrDiffuseTexture);
        constants.metalRoughOrSpecularTextureIndex = GetBindlessTextureIndex(metalRoughOrSpecularTexture);
        constants.normalTextureIndex = GetBindlessTextureIndex(normalTexture);
        constants.emissiveTextureIndex = GetBindlessTextureIndex(emissiveTexture);
        constants.occlusionTextureIndex = GetBindlessTextureIndex(occlusionTexture);
        constants.transmissionTextureIndex = GetBindlessTextureIndex(transmissionTexture);
        constants.opacityTextureIndex = GetBindlessTextureIndex(opacityTexture);

        constants.padding1 = uint3(0, 0, 0);
    }

    bool Material::SetProperty(const std::string& name, const dm::float4& value)
    {
#define FLOAT3_PROPERTY(pname) if (name == #pname) { pname = value.xyz(); dirty = true; return true; }
#define FLOAT_PROPERTY(pname) if (name == #pname) { pname = value.x; dirty = true; return true; }
#define FLOAT2_PROPERTY(pname) if (name == #pname) { pname = value.xy(); dirty = true; return true; }
#define BOOL_PROPERTY(pname) if (name == #pname) { pname = (value.x > 0.5f); dirty = true; return true; }
        FLOAT3_PROPERTY(baseOrDiffuseColor);
        FLOAT3_PROPERTY(specularColor);
        FLOAT3_PROPERTY(emissiveColor);
        FLOAT_PROPERTY(emissiveIntensity);
        FLOAT_PROPERTY(metalness);
        FLOAT_PROPERTY(roughness);
        FLOAT_PROPERTY(opacity);
        FLOAT_PROPERTY(alphaCutoff);
        FLOAT_PROPERTY(transmissionFactor);
        FLOAT_PROPERTY(normalTextureScale);
        FLOAT_PROPERTY(occlusionStrength);
        FLOAT2_PROPERTY(normalTextureTransformScale);
        BOOL_PROPERTY(enableBaseOrDiffuseTexture);
        BOOL_PROPERTY(enableMetalRoughOrSpecularTexture);
        BOOL_PROPERTY(enableNormalTexture);
        BOOL_PROPERTY(enableEmissiveTexture);
        BOOL_PROPERTY(enableOcclusionTexture);
        BOOL_PROPERTY(enableTransmissionTexture);
        BOOL_PROPERTY(enableOpacityTexture);
#undef FLOAT3_PROPERTY
#undef FLOAT_PROPERTY
#undef BOOL_PROPERTY

        return false;
    }
}
