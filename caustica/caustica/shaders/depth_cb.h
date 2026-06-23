#ifndef DEPTH_CB_H
#define DEPTH_CB_H

#define DEPTH_SPACE_MATERIAL 0
#define DEPTH_BINDING_MATERIAL_DIFFUSE_TEXTURE 0
#define DEPTH_BINDING_MATERIAL_OPACITY_TEXTURE 1
#define DEPTH_BINDING_MATERIAL_CONSTANTS 0

#define DEPTH_SPACE_INPUT 1
#define DEPTH_BINDING_PUSH_CONSTANTS 1
#define DEPTH_BINDING_INSTANCE_BUFFER 10
#define DEPTH_BINDING_VERTEX_BUFFER 11

#define DEPTH_SPACE_VIEW 2
#define DEPTH_BINDING_VIEW_CONSTANTS 2
#define DEPTH_BINDING_MATERIAL_SAMPLER 0

struct DepthPassConstants
{
    float4x4    matWorldToClip;
};

struct DepthPushConstants
{
    uint        startInstanceLocation;
    uint        startVertexLocation;
    uint        positionOffset;
    uint        texCoordOffset;
};

#endif // DEPTH_CB_H