#include "LocalConfig.h"

#include <EditorUI.h>
#include "SceneEditor.h"

namespace caustica::editor
{

void LocalConfig::PreferredSceneOverride(std::string& preferredScene)
{
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "REF_VS_REALTIME")
    {
        preferredScene = "kitchen-with-test-stuff.scene.json";
    }
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "DENOISER_TUNING")
    {
        //preferredScene = "transparent-machines.scene.json";
    }
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "GENERIC_STABLE_LIGHTS")
    {
        preferredScene = "bistro-programmer-art.scene.json";
        //preferredScene = "kitchen-with-test-stuff.scene.json";
    }
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "PROC_SKY_TESTING")
    {
        preferredScene = "programmer-art-proc-sky.scene.json";
    }
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "ENVMAP_ONLY_TESTING")
    {
        preferredScene = "bistro-programmer-art.scene.json";
        // preferredScene = "kitchen.scene.json";
        // preferredScene = "programmer-art.scene.json";
    }
}

void LocalConfig::PostAppInit(caustica::render::RenderSessionState& sampleUI)
{
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "REF_VS_REALTIME")
    {
#if 0 // test for making reference pixel-identical to realtime
        sampleUI.settings.AccumulationAA = false;
        sampleUI.settings.AccumulationTarget = 4;
        sampleUI.settings.RealtimeSamplesPerPixel = sampleUI.settings.AccumulationTarget;
        sampleUI.settings.RealtimeAA = 0;
        sampleUI.settings.RealtimeDenoiser = false;
        sampleUI.settings.DbgFreezeRealtimeNoiseSeed = false;
        sampleUI.settings.StablePlanesActiveCount = 1;
        sampleUI.settings.AllowPrimarySurfaceReplacement = false;
        sampleUI.settings.RealtimeDiffuseBounceCount = sampleUI.settings.ReferenceDiffuseBounceCount;
        sampleUI.settings.RealtimeFireflyFilterEnabled = false;
        sampleUI.settings.ReferenceFireflyFilterEnabled = false;
        //sampleUI.settings.EnableRussianRoulette = false;
        //sampleUI.settings.BounceCount = 1;
#endif
    }

    // This disables...
    //  * ReSTIR DI & ReSTIR GI 
    //  * AutoExposure 
    //  * Stable Planes (set to 1) 
    // ...and increases brute force sampling - useful for denoiser tuning as it removes temporal issues and prevents stable planes from hiding issues.
    // Once denoiser works well, try enabling things one by one (and reducing NEE & global samples back to 1)
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "DENOISER_TUNING")
    {
        sampleUI.settings.RealtimeMode = true;
        sampleUI.settings.UseReSTIRDI = false;           // avoid any temporal issues from DI
        sampleUI.settings.UseReSTIRGI = false;           // avoid any temporal issues from GI
        sampleUI.settings.UseReSTIRPT = false;           // avoid any temporal issues from PT
        sampleUI.settings.ToneMappingParams.autoExposure = false;    // for stable before/after image comparisons
        sampleUI.settings.StablePlanesActiveCount = 1;   // disable SPs - we want as good raw denoising as possible without SPs hiding any issues
        sampleUI.settings.RealtimeSamplesPerPixel = 2;   // boost global samples
        sampleUI.settings.NEEType = 1;              // avoid any temporal issues from ReGIR (due to presampling + multiple full local samples)
        sampleUI.settings.RealtimeAA = 1;
    }

    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "ENVMAP_ONLY_TESTING")
    {
        sampleUI.settings.UseReSTIRDI = false;
        sampleUI.settings.UseReSTIRGI = false;
        sampleUI.settings.UseReSTIRPT = false;
        sampleUI.settings.ToneMappingParams.autoExposure = false;
        sampleUI.settings.RealtimeAA = 0;
        sampleUI.settings.StandaloneDenoiser = false;
        //sampleUI.settings.ToneMappingParams.exposureCompensation = 5.2f;
        sampleUI.settings.EnableAnimations = false;
        sampleUI.settings.ReferenceFireflyFilterEnabled = false;

        sampleUI.settings.RealtimeMode = false;
    }

    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "GENERIC_STABLE_LIGHTS")
    {
#if 0
        sampleUI.settings.AccumulationTarget = 2048;
        sampleUI.settings.RealtimeMode = false;
        sampleUI.settings.UseReSTIRDI = false;
        sampleUI.settings.UseReSTIRGI = false;
        sampleUI.settings.UseReSTIRPT = false;
        sampleUI.settings.ToneMappingParams.autoExposure = false;
        sampleUI.settings.StablePlanesActiveCount = 1;
        sampleUI.settings.ReferenceFireflyFilterEnabled = false;
        sampleUI.settings.EnableRussianRoulette = false;
#else
        sampleUI.settings.UseReSTIRDI = false;
        sampleUI.settings.UseReSTIRGI = false;
        sampleUI.settings.UseReSTIRPT = false;
        sampleUI.settings.ToneMappingParams.autoExposure = false;
        sampleUI.settings.RealtimeAA = 0;
        sampleUI.settings.StandaloneDenoiser = false;
        sampleUI.settings.ReferenceFireflyFilterEnabled = false; //true;
        sampleUI.settings.ReferenceFireflyFilterThreshold = 1.0f;
        sampleUI.settings.RealtimeFireflyFilterEnabled = false; //true;
        sampleUI.settings.RealtimeFireflyFilterThreshold = 1.0f;
        sampleUI.settings.DiffuseBounceCount = 2;
        //sampleUI.settings.DbgFreezeRealtimeNoiseSeed = false;
        sampleUI.settings.EnableBloom = false;
        //sampleUI.settings.ToneMappingParams.exposureCompensation = 1.5f;
#endif
        //sampleUI.settings.EnvironmentMapParams.enabled = false;
        //sampleUI.settings.ToneMappingParams.exposureCompensation = 5.2f;
        sampleUI.settings.EnableAnimations = false;
    }

}

void LocalConfig::PostSceneLoad(SceneEditor& sample, caustica::render::RenderSessionState& sampleUI, EditorUIState& editorUI)
{
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "ENVMAP_ONLY_TESTING")
    {
        for (int i = 0; editorUI.TogglableNodes != nullptr && i < editorUI.TogglableNodes->size(); i++)
        {
            TogglableNode& node = (*editorUI.TogglableNodes)[i];
            if (node.UIName == "Ceiling")
                node.SetSelected(false);
        }
    }
    (void)sample;
    (void)sampleUI;
}

void LocalConfig::PostMaterialLoad(caustica::Material& mat)
{
#if 0 // convert transmissive to white opaque
    if (mat.domain == MaterialDomain::Transmissive || mat.domain == MaterialDomain::TransmissiveAlphaBlended || mat.domain == MaterialDomain::TransmissiveAlphaTested)
    {
        mat.baseOrDiffuseColor = float3(1, 1, 1);
        mat.enableBaseOrDiffuseTexture = false;
    }
    if (mat.domain == MaterialDomain::Transmissive) mat.domain = MaterialDomain::Opaque;
    if (mat.domain == MaterialDomain::TransmissiveAlphaBlended) mat.domain = MaterialDomain::AlphaBlended;
    if (mat.domain == MaterialDomain::TransmissiveAlphaTested)  mat.domain = MaterialDomain::AlphaTested;
#endif
#if 1 // disable emissive lights - this doesn't work if there's any animation changing brightness
    if (CAUSTICA_LOCAL_CONFIG_ID_STRING == "ENVMAP_ONLY_TESTING" /*|| CAUSTICA_LOCAL_CONFIG_ID_STRING == "GENERIC_STABLE_LIGHTS"*/ )
        mat.emissiveIntensity = 0.0f;
#endif
}

} // namespace caustica::editor

