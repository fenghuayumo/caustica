#include "PathTracer/PathTracerTypes.hlsli"

#include "Bindings/ShaderResourceBindings.hlsli"

// Development/debug post-process raygen helpers. Production post-process is
// elsewhere under PostProcess*.

#ifdef PP_EDGE_DETECTION
float3 LoadLDR(uint2 pixelPos)
{
    return t_LdrColorScratch[pixelPos].rgb;
}
void SaveLDR(uint2 pixelPos, float3 linearColor)
{
    u_PostTonemapOutputColor[pixelPos].rgb = LinearToSRGB(linearColor);
}
[shader("raygeneration")]
void RAYGEN_ENTRY()
{
    uint2 pixelPos = DispatchRaysIndex().xy;
    int offX = 1; int offY = 1;

	float3 s00 = LoadLDR(pixelPos + int2( -offX, -offY ));
	float3 s01 = LoadLDR(pixelPos + int2(     0, -offY ));
	float3 s02 = LoadLDR(pixelPos + int2(  offX, -offY ));
	float3 s10 = LoadLDR(pixelPos + int2( -offX,  0    ));
	float3 s12 = LoadLDR(pixelPos + int2(  offX,  0    ));
	float3 s20 = LoadLDR(pixelPos + int2( -offX,  offY ));
	float3 s21 = LoadLDR(pixelPos + int2(     0,  offY ));
	float3 s22 = LoadLDR(pixelPos + int2(  offX,  offY ));

// add reorder threads here? convert to lpfloat?
	
	float3 sobelX = s00 + 2 * s10 + s20 - s02 - 2 * s12 - s22;
	float3 sobelY = s00 + 2 * s01 + s02 - s20 - 2 * s21 - s22;

	float3 edgeSqr = (sobelX * sobelX + sobelY * sobelY);
	
    const float kThreshold = asfloat(g_MiniConst.params[0]);

	float3 edgeColor = 1.xxx-(edgeSqr > kThreshold.xxx * kThreshold.xxx);
    SaveLDR( pixelPos, saturate(edgeColor) );
}
#endif

[shader("miss")]
void MISS_ENTRY(inout PathPayload path : SV_RayPayload)
{
}
