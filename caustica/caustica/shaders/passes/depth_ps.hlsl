#pragma pack_matrix(row_major)

#include <shaders/depth_cb.h>
#include <shaders/material_cb.h>
#include <shaders/scene_material.hlsli>
#include <shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(MaterialConstants, g_Material, DEPTH_BINDING_MATERIAL_CONSTANTS, DEPTH_SPACE_MATERIAL);

Texture2D t_BaseOrDiffuse       : REGISTER_SRV(DEPTH_BINDING_MATERIAL_DIFFUSE_TEXTURE, DEPTH_SPACE_MATERIAL);
Texture2D t_Opacity             : REGISTER_SRV(DEPTH_BINDING_MATERIAL_OPACITY_TEXTURE, DEPTH_SPACE_MATERIAL);
SamplerState s_MaterialSampler  : REGISTER_SAMPLER(DEPTH_BINDING_MATERIAL_SAMPLER, DEPTH_SPACE_VIEW);

void main(
    in float4 i_position : SV_Position,
	in float2 i_texCoord : TEXCOORD
)
{
    MaterialTextureSample textures = DefaultMaterialTextures();
    if ((g_Material.flags & MaterialFlags_UseBaseOrDiffuseTexture) != 0)
        textures.baseOrDiffuse = t_BaseOrDiffuse.Sample(s_MaterialSampler, i_texCoord);
    if ((g_Material.flags & MaterialFlags_UseOpacityTexture) != 0)
        textures.opacity = t_Opacity.Sample(s_MaterialSampler, i_texCoord).r;

    MaterialSample materialSample = EvaluateSceneMaterial(/* normal = */ float3(1, 0, 0),
        /* tangent = */ float4(0, 1, 0, 0), g_Material, textures);

    clip(materialSample.opacity - g_Material.alphaCutoff);
}
