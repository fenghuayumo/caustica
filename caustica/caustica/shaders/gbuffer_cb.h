#ifndef GBUFFER_CB_H
#define GBUFFER_CB_H

#include "view_cb.h"

#define GBUFFER_SPACE_MATERIAL 0
#define GBUFFER_BINDING_MATERIAL_CONSTANTS 0
#define GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE 0
#define GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE 1
#define GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE 2
#define GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE 3
#define GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE 4
#define GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE 5
#define GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE 6

#define GBUFFER_SPACE_INPUT 1
#define GBUFFER_BINDING_PUSH_CONSTANTS 1
#define GBUFFER_BINDING_INSTANCE_BUFFER 10
#define GBUFFER_BINDING_VERTEX_BUFFER 11

#define GBUFFER_SPACE_VIEW 2
#define GBUFFER_BINDING_VIEW_CONSTANTS 2
#define GBUFFER_BINDING_MATERIAL_SAMPLER 0

struct GBufferFillConstants
{
    PlanarViewConstants view;
    PlanarViewConstants viewPrev;
};

struct GBufferPushConstants
{
    uint        startInstanceLocation;
    uint        startVertexLocation;
    uint        positionOffset;
    uint        prevPositionOffset;
    uint        texCoordOffset;
    uint        normalOffset;
    uint        tangentOffset;
};

#endif // GBUFFER_CB_H