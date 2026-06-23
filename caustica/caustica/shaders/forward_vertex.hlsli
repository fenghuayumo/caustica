#ifndef FORWARD_VERTEX_HLSLI
#define FORWARD_VERTEX_HLSLI

struct SceneVertex
{
    float3 pos : POS;
    float3 prevPos : PREV_POS;
    float2 texCoord : TEXCOORD;
    centroid float3 normal : NORMAL;
    centroid float4 tangent : TANGENT;
};

#endif