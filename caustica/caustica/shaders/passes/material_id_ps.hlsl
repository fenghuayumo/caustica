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
#include <shaders/forward_vertex.hlsli>
#include <shaders/material_bindings.hlsli>
#include <shaders/scene_material.hlsli>
#include <shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(GBufferFillConstants, c_GBuffer, GBUFFER_BINDING_VIEW_CONSTANTS, GBUFFER_SPACE_VIEW);
DECLARE_PUSH_CONSTANTS(GBufferPushConstants, g_Push, GBUFFER_BINDING_PUSH_CONSTANTS, GBUFFER_SPACE_INPUT);

void main(
    in float4 i_position : SV_Position,
    in SceneVertex i_vtx,
    in uint i_instance : INSTANCE,
    out uint4 o_output : SV_Target0
)
{
#if ALPHA_TESTED
    MaterialTextureSample textures = SampleMaterialTexturesAuto(i_vtx.texCoord, g_Material.normalTextureTransformScale);

    MaterialSample surface = EvaluateSceneMaterial(i_vtx.normal, i_vtx.tangent, g_Material, textures);
    
    clip(surface.opacity - g_Material.alphaCutoff);
#endif

    o_output.x = uint(g_Material.materialID);
    o_output.y = g_Push.startInstanceLocation + i_instance;
    o_output.zw = 0;
}
