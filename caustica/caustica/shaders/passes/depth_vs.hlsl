#pragma pack_matrix(row_major)

#include <shaders/depth_cb.h>
#include <shaders/bindless.h>
#include <shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(DepthPassConstants, g_Depth, DEPTH_BINDING_VIEW_CONSTANTS, DEPTH_SPACE_VIEW);

void input_assembler(
	in float3 i_pos : POSITION,
    in float2 i_texCoord : TEXCOORD,
    in float3x4 i_instanceMatrix : TRANSFORM,
	in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out float2 o_texCoord : TEXCOORD)
{
    float3x4 instanceMatrix = i_instanceMatrix;

	float4 worldPos = float4(mul(instanceMatrix, float4(i_pos, 1.0)), 1.0);
	o_position = mul(worldPos, g_Depth.matWorldToClip);

    o_texCoord = i_texCoord;
}

// Use a raw buffer on DX11 to avoid adding the StructuredBuffer flag to the instance buffer.
// On DX11, a structured buffer cannot be used as a vertex buffer, and there should be compatibility with other passes.
// On DX12, using a structured buffer results in more optimal code being generated.
#ifdef TARGET_D3D11
ByteAddressBuffer t_Instances : REGISTER_SRV(DEPTH_BINDING_INSTANCE_BUFFER, DEPTH_SPACE_INPUT);
#else
StructuredBuffer<InstanceData> t_Instances : REGISTER_SRV(DEPTH_BINDING_INSTANCE_BUFFER, DEPTH_SPACE_INPUT);
#endif
ByteAddressBuffer t_Vertices : REGISTER_SRV(DEPTH_BINDING_VERTEX_BUFFER, DEPTH_SPACE_INPUT);

DECLARE_PUSH_CONSTANTS(DepthPushConstants, g_Push, DEPTH_BINDING_PUSH_CONSTANTS, DEPTH_SPACE_INPUT);


// Version of the vertex shader that uses buffer loads to read vertex attributes and transforms.
void buffer_loads(
    in uint i_vertex : SV_VertexID,
	in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out float2 o_texCoord : TEXCOORD
)
{
    i_instance += g_Push.startInstanceLocation;
    i_vertex += g_Push.startVertexLocation;

#ifdef TARGET_D3D11
    const InstanceData instance = LoadInstanceData(t_Instances, i_instance * c_SizeOfInstanceData);
#else
    const InstanceData instance = t_Instances[i_instance];
#endif

    float3 pos = asfloat(t_Vertices.Load3(g_Push.positionOffset + i_vertex * c_SizeOfPosition));
    float2 texCoord = asfloat(t_Vertices.Load2(g_Push.texCoordOffset + i_vertex * c_SizeOfTexcoord));
 
    float3 worldPos = mul(instance.transform, float4(pos, 1.0));
    o_texCoord = texCoord;

    o_position = mul(float4(worldPos, 1.0), g_Depth.matWorldToClip);
}
