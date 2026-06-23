#ifndef POLYMORPHIC_LIGHT_RTXDI_HLSLI
#define POLYMORPHIC_LIGHT_RTXDI_HLSLI

#include "HelperFunctions.hlsli"
#include <shaders/PathTracer/Utils/Utils.hlsli>
#include <shaders/PathTracer/Lighting/LightShaping.hlsli>
#include <shaders/PathTracer/Utils/ColorHelpers.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <shaders/PathTracer/PathTracerHelpers.hlsli>
#include <Rtxdi/Utils/Checkerboard.hlsli>

// This is a adapter for PolymorphicLight, enabling features as needed by RTXDI

// Polymorphic light config - RTXDI will also need ENV 
#define POLYLIGHT_SPHERE_ENABLE         1
#define POLYLIGHT_POINT_ENABLE          1
#define POLYLIGHT_TRIANGLE_ENABLE       1
#define POLYLIGHT_DIRECTIONAL_ENABLE    1   // probably not needed as baked in envmap but this need testing
#define POLYLIGHT_ENV_ENABLE            1
#define POLYLIGHT_QT_ENV_ENABLE         0   // environment map quad tree in equal area octahedral mapping
#define POLYLIGHT_CONFIGURED

#include <shaders/PathTracer/Lighting/PolymorphicLight.hlsli>


PolymorphicLightSample EnvironmentLight::CalcSample(in const float2 random, in const float3 viewerPosition)
{
    PolymorphicLightSample pls;
       
    EnvMapSampler envMapSampler = EnvMapSampler::make( s_EnvironmentMapImportanceSampler, t_EnvironmentMapImportanceMap, g_Const.envMapImportanceSamplingParams,
                                                        t_EnvironmentMap, s_EnvironmentMapSampler, g_Const.envMapSceneParams );

    float3 worldDir = Decode_Oct( random );
    pls.Position = viewerPosition + worldDir * DISTANT_LIGHT_DISTANCE;
    pls.Normal = -worldDir;
    pls.Radiance = envMapSampler.Eval(worldDir);
    pls.SolidAnglePdf = envMapSampler.MIPDescentEvalPdf(worldDir);
        
    return pls;
}    

inline EnvironmentLight EnvironmentLight::Create(in const PolymorphicLightInfoFull lightInfo)
{
    EnvironmentLight envLight;

    return envLight;
}

#endif // POLYMORPHIC_LIGHT_RTXDI_HLSLI