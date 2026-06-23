#include <shaders/binding_helpers.hlsli>

Buffer<uint> t_Source : register(t0);
RWTexture2D<float> u_Dest : register(u0);

struct Constants {
    float scale;
};

DECLARE_PUSH_CONSTANTS(Constants, g_Const, 0, 0);

// Converts the exposure (adapted luminance) value computed by Donut's ToneMappingPass
// into a texture consumable by DLSS.

[numthreads(1, 1, 1)]
void main()
{
    float adaptedLuminance = asfloat(t_Source[0]);

    float exposure = 1.0;
    if (adaptedLuminance > 0)
    {
        // Use the conversion suggested in the DLSS Programming Guide,
        // section 3.9 "Exposure Parameter"
        const float midGray = 0.18;
        exposure = midGray / (adaptedLuminance * (1.0 - midGray));
    }

    exposure *= g_Const.scale;

    u_Dest[int2(0, 0)] = exposure;
}