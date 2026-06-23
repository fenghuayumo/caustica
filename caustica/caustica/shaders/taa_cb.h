#ifndef TAA_CB_H
#define TAA_CB_H

struct TemporalAntiAliasingConstants
{
    float4x4 reprojectionMatrix;

    float2 inputViewOrigin;
    float2 inputViewSize;

    float2 outputViewOrigin;
    float2 outputViewSize;

    float2 inputPixelOffset;
    float2 outputTextureSizeInv;

    float2 inputOverOutputViewSize;
    float2 outputOverInputViewSize;

    float clampingFactor;
    float newFrameWeight;
    float pqC;
    float invPqC;

    uint stencilMask;
    uint useHistoryClampRelax;
    uint padding0;
    uint padding1;
};

#endif // TAA_CB_H