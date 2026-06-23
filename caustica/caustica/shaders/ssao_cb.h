#ifndef SSAO_CB_H
#define SSAO_CB_H

#include "shaders/view_cb.h"

struct SsaoConstants
{
    PlanarViewConstants view;
    
    float2      clipToView;
    float2      invQuantizedGbufferSize;

    int2        quantizedViewportOrigin;
    float       amount;
    float       invBackgroundViewDepth;
    float       radiusWorld;
    float       surfaceBias;

    float       radiusToScreen;
    float       powerExponent;
};

#endif // SSAO_CB_H