// In-place color-space conversion around vk-compatible display-referred splat blending.
// Each thread only accesses its own pixel, so the UAV read/modify/write is race-free.

#include <shaders/binding_helpers.hlsli>

struct GaussianSplatColorSpaceConstants
{
    uint mode; // 0: linear -> sRGB, 1: sRGB -> linear
    uint width;
    uint height;
    uint padding;
};

VK_PUSH_CONSTANT ConstantBuffer<GaussianSplatColorSpaceConstants> g_Const : register(b0);
RWTexture2D<float4> u_Color : register(u0);

float LinearToSrgb(float value)
{
    if (value <= 0.0031308f)
        return 12.92f * value;
    return 1.055f * pow(max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
}

float SrgbToLinear(float value)
{
    if (value <= 0.04045f)
        return value / 12.92f;
    return pow((max(value, 0.0f) + 0.055f) / 1.055f, 2.4f);
}

float3 LinearToSrgb(float3 value)
{
    return float3(
        LinearToSrgb(value.r),
        LinearToSrgb(value.g),
        LinearToSrgb(value.b));
}

float3 SrgbToLinear(float3 value)
{
    return float3(
        SrgbToLinear(value.r),
        SrgbToLinear(value.g),
        SrgbToLinear(value.b));
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= g_Const.width || pixel.y >= g_Const.height)
        return;

    float4 color = u_Color[pixel];
    color.rgb = g_Const.mode == 0
        ? LinearToSrgb(color.rgb)
        : SrgbToLinear(color.rgb);
    u_Color[pixel] = color;
}
