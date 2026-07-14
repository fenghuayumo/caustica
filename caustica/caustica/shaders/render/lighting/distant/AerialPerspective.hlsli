#ifndef __AERIAL_PERSPECTIVE_HLSLI__
#define __AERIAL_PERSPECTIVE_HLSLI__

#include <shaders/view_cb.h>
#include "SkyAtmosphereCommon.hlsli"

struct AerialPerspectiveConstants
{
    PlanarViewConstants View;
    AtmosphereParameters Atmosphere;

    float3 SunDir;
    float CameraHeightKm;

    float3 SunIlluminance;
    float WorldToKilometers;

    float3 RadianceMultiplier;
    float MaxDistanceKm;

    float3 AtmosphereBasisXWorld;
    float _padBasisX;

    float3 AtmosphereBasisYWorld;
    float _padBasisY;

    float3 AtmosphereBasisZWorld;
    float _padBasisZ;

    uint2 OutputSize;
    uint ReverseDepth;
    uint SampleCount;

    float _pad0;
    float _pad1;
    float _pad2;
    float _pad3;
};

#endif
