#pragma pack_matrix(row_major)

#include <shaders/gbuffer_cb.h>

// Declare the constants that drive material bindings in 'material_bindings.hlsli'
// to match the bindings explicitly declared in 'gbuffer_cb.h'.

#define MATERIAL_REGISTER_SPACE     GBUFFER_SPACE_MATERIAL
#define MATERIAL_CB_SLOT            GBUFFER_BINDING_MATERIAL_CONSTANTS
#define MATERIAL_DIFFUSE_SLOT       GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE
#define MATERIAL_SPECULAR_SLOT      GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE
#define MATERIAL_NORMALS_SLOT       GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE
#define MATERIAL_EMISSIVE_SLOT      GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE
#define MATERIAL_OCCLUSION_SLOT     GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE
#define MATERIAL_TRANSMISSION_SLOT  GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE
#define MATERIAL_OPACITY_SLOT       GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE

#define MATERIAL_SAMPLER_REGISTER_SPACE GBUFFER_SPACE_VIEW
#define MATERIAL_SAMPLER_SLOT           GBUFFER_BINDING_MATERIAL_SAMPLER

#include <shaders/scene_material.hlsli>
#include <shaders/material_bindings.hlsli>
#include <shaders/motion_vectors.hlsli>
#include <shaders/forward_vertex.hlsli>
#include <shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(GBufferFillConstants, c_GBuffer, GBUFFER_BINDING_VIEW_CONSTANTS, GBUFFER_SPACE_VIEW);

void main(
    in float4 i_position : SV_Position,
	in SceneVertex i_vtx,
    in bool i_isFrontFace : SV_IsFrontFace,
    out float4 o_channel0 : SV_Target0,
    out float4 o_channel1 : SV_Target1,
    out float4 o_channel2 : SV_Target2,
    out float4 o_channel3 : SV_Target3
#if MOTION_VECTORS
    , out float3 o_motion : SV_Target4
#endif
)
{
    MaterialTextureSample textures = SampleMaterialTexturesAuto(i_vtx.texCoord, g_Material.normalTextureTransformScale);

    MaterialSample surface = EvaluateSceneMaterial(i_vtx.normal, i_vtx.tangent, g_Material, textures);

#if ALPHA_TESTED
    if (g_Material.domain != MaterialDomain_Opaque)
        clip(surface.opacity - g_Material.alphaCutoff);
#endif

    if (!i_isFrontFace)
        surface.shadingNormal = -surface.shadingNormal;

    o_channel0.xyz = surface.diffuseAlbedo;
    o_channel0.w = surface.opacity;
    o_channel1.xyz = surface.specularF0;
    o_channel1.w = surface.occlusion;
    o_channel2.xyz = surface.shadingNormal;
    o_channel2.w = surface.roughness;
    o_channel3.xyz = surface.emissiveColor;
    o_channel3.w = 0;

#if MOTION_VECTORS
    o_motion = GetMotionVector(i_position.xyz, i_vtx.prevPos, c_GBuffer.view, c_GBuffer.viewPrev);
#endif
}
