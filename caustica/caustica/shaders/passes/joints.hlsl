#include <shaders/view_cb.h>
#include <shaders/packing.hlsli>
#include <shaders/binding_helpers.hlsli>

// simple line drawing shader for the joints render pass

cbuffer c_Constants : register(b0 VK_DESCRIPTOR_SET(0))
{
    PlanarViewConstants g_Constants;
};

struct VertexAttributes
{
    float3 position : POSITION;
    uint color : COLOR;
};

void main_vs(
    in VertexAttributes i_vtx
    , in uint i_instanceID : SV_InstanceID
    , out float4 o_position : SV_Position
    , out float3 o_color : COLOR
)
{
    o_position = mul(float4(i_vtx.position, 1), g_Constants.matWorldToClip);

    o_color = Unpack_RGB8_SNORM(i_vtx.color);
}

void main_ps(
    in float4 i_position : SV_Position
    , in float3 i_color : COLOR
    , out float4 o_color : SV_Target0
)
{
    o_color = float4(i_color, 1);
}