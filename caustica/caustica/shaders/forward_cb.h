#ifndef FORWARD_CB_H
#define FORWARD_CB_H

#include "light_cb.h"
#include "view_cb.h"

#define FORWARD_MAX_LIGHTS 16
#define FORWARD_MAX_SHADOWS 16
#define FORWARD_MAX_LIGHT_PROBES 16

#define FORWARD_SPACE_MATERIAL 0
#define FORWARD_BINDING_MATERIAL_CONSTANTS 0
#define FORWARD_BINDING_MATERIAL_DIFFUSE_TEXTURE 0
#define FORWARD_BINDING_MATERIAL_SPECULAR_TEXTURE 1
#define FORWARD_BINDING_MATERIAL_NORMAL_TEXTURE 2
#define FORWARD_BINDING_MATERIAL_EMISSIVE_TEXTURE 3
#define FORWARD_BINDING_MATERIAL_OCCLUSION_TEXTURE 4
#define FORWARD_BINDING_MATERIAL_TRANSMISSION_TEXTURE 5
#define FORWARD_BINDING_MATERIAL_OPACITY_TEXTURE 6

#define FORWARD_SPACE_INPUT 1
#define FORWARD_BINDING_PUSH_CONSTANTS 1
#define FORWARD_BINDING_INSTANCE_BUFFER 10
#define FORWARD_BINDING_VERTEX_BUFFER 11

#define FORWARD_SPACE_VIEW 2
#define FORWARD_BINDING_VIEW_CONSTANTS 2

#define FORWARD_SPACE_SHADING 3
#define FORWARD_BINDING_LIGHT_CONSTANTS 3
#define FORWARD_BINDING_SHADOW_MAP_TEXTURE 20
#define FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE 21
#define FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE 22
#define FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE 23
#define FORWARD_BINDING_MATERIAL_SAMPLER 0
#define FORWARD_BINDING_SHADOW_MAP_SAMPLER 1
#define FORWARD_BINDING_LIGHT_PROBE_SAMPLER 2
#define FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER 3


struct ForwardShadingViewConstants
{
    PlanarViewConstants view;
};

struct ForwardShadingLightConstants
{
    float2      shadowMapTextureSize;
    float2      shadowMapTextureSizeInv;
    float4      ambientColorTop;
    float4      ambientColorBottom;

    uint2       padding;
    uint        numLights;
    uint        numLightProbes;

    LightConstants lights[FORWARD_MAX_LIGHTS];
    ShadowConstants shadows[FORWARD_MAX_SHADOWS];
    LightProbeConstants lightProbes[FORWARD_MAX_LIGHT_PROBES];
};

struct ForwardPushConstants
{
    uint        startInstanceLocation;
    uint        startVertexLocation;
    uint        positionOffset;
    uint        texCoordOffset;
    uint        normalOffset;
    uint        tangentOffset;
};

#endif // FORWARD_CB_H