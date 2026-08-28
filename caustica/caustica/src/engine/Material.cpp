#include <scene/SceneTypes.h>

namespace caustica
{
    bool Material::setProperty(const std::string& name, const dm::float4& value)
    {
#define FLOAT3_PROPERTY(pname) if (name == #pname) { pname = value.xyz(); return true; }
#define FLOAT_PROPERTY(pname) if (name == #pname) { pname = value.x; return true; }
#define FLOAT2_PROPERTY(pname) if (name == #pname) { pname = value.xy(); return true; }
#define BOOL_PROPERTY(pname) if (name == #pname) { pname = (value.x > 0.5f); return true; }
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
#undef FLOAT2_PROPERTY
#undef BOOL_PROPERTY

        return false;
    }
}
