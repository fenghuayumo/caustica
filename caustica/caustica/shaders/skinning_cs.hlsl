#pragma pack_matrix(row_major)

#include <shaders/bindless.h>
#include <shaders/binding_helpers.hlsli>
#include <shaders/packing.hlsli>
#include <shaders/skinning_cb.h>

ByteAddressBuffer t_VertexBuffer : register(t0);
ByteAddressBuffer t_JointMatrices : register(t1);

RWByteAddressBuffer u_VertexBuffer : register(u0);

DECLARE_PUSH_CONSTANTS(SkinningConstants, g_Const, 0, 0);

[numthreads(256, 1, 1)]
void main(in uint i_globalIdx : SV_DispatchThreadID)
{
	if (i_globalIdx >= g_Const.numVertices)
		return;

	float3 position = asfloat(t_VertexBuffer.Load3(i_globalIdx * c_SizeOfPosition + g_Const.inputPositionOffset));
	float4 normal = 0;
	float4 tangent = 0;
	float2 texCoord1 = 0;
	float2 texCoord2 = 0;

	if (g_Const.flags & SkinningFlag_Normals)
		normal = Unpack_RGBA8_SNORM(t_VertexBuffer.Load(i_globalIdx * c_SizeOfNormal + g_Const.inputNormalOffset));

	if (g_Const.flags & SkinningFlag_Tangents)
		tangent = Unpack_RGBA8_SNORM(t_VertexBuffer.Load(i_globalIdx * c_SizeOfNormal + g_Const.inputTangentOffset));

	if (g_Const.flags & SkinningFlag_TexCoord1)
		texCoord1 = asfloat(t_VertexBuffer.Load2(i_globalIdx * c_SizeOfTexcoord + g_Const.inputTexCoord1Offset));

	if (g_Const.flags & SkinningFlag_TexCoord2)
		texCoord2 = asfloat(t_VertexBuffer.Load2(i_globalIdx * c_SizeOfTexcoord + g_Const.inputTexCoord2Offset));

	uint2 jointIndicesPacked = t_VertexBuffer.Load2(i_globalIdx * c_SizeOfJointIndices + g_Const.inputJointIndexOffset);
	uint4 jointIndices = uint4(
		jointIndicesPacked.x & 0xffff, jointIndicesPacked.x >> 16,
		jointIndicesPacked.y & 0xffff, jointIndicesPacked.y >> 16);
	float4 jointWeights = asfloat(t_VertexBuffer.Load4(i_globalIdx * c_SizeOfJointWeights + g_Const.inputJointWeightOffset));

	float4x4 jointMatrix = 0;
	[unroll]
	for (int i = 0; i < 4; i++)
	{
		if (jointWeights[i] > 0)
		{
			uint index = jointIndices[i];
			float4x4 currentMatrix;
			currentMatrix[0] = asfloat(t_JointMatrices.Load4(index * 64 + 0));
			currentMatrix[1] = asfloat(t_JointMatrices.Load4(index * 64 + 16));
			currentMatrix[2] = asfloat(t_JointMatrices.Load4(index * 64 + 32));
			currentMatrix[3] = asfloat(t_JointMatrices.Load4(index * 64 + 48));
			jointMatrix += currentMatrix * jointWeights[i];
		}
	}

	position = mul(float4(position, 1.0), jointMatrix).xyz;
	normal.xyz = normalize(mul(float4(normal.xyz, 0.0), jointMatrix).xyz);
	tangent.xyz = normalize(mul(float4(tangent.xyz, 0.0), jointMatrix).xyz);

	float3 prevPosition;
	if (g_Const.flags & SkinningFlag_FirstFrame) 
		prevPosition = position;
	else
		prevPosition = asfloat(u_VertexBuffer.Load3(i_globalIdx * c_SizeOfPosition + g_Const.outputPositionOffset));
	u_VertexBuffer.Store3(i_globalIdx * c_SizeOfPosition + g_Const.outputPrevPositionOffset, asuint(prevPosition));

	u_VertexBuffer.Store3(i_globalIdx * c_SizeOfPosition + g_Const.outputPositionOffset, asuint(position));
	
	if (g_Const.flags & SkinningFlag_Normals)
		u_VertexBuffer.Store(i_globalIdx * c_SizeOfNormal + g_Const.outputNormalOffset, Pack_RGBA8_SNORM(normal));
	
	if (g_Const.flags & SkinningFlag_Tangents)
		u_VertexBuffer.Store(i_globalIdx * c_SizeOfNormal + g_Const.outputTangentOffset, Pack_RGBA8_SNORM(tangent));
	
	if (g_Const.flags & SkinningFlag_TexCoord1)
		u_VertexBuffer.Store2(i_globalIdx * c_SizeOfTexcoord + g_Const.outputTexCoord1Offset, asuint(texCoord1));

	if (g_Const.flags & SkinningFlag_TexCoord2)
		u_VertexBuffer.Store2(i_globalIdx * c_SizeOfTexcoord + g_Const.outputTexCoord2Offset, asuint(texCoord2));
}