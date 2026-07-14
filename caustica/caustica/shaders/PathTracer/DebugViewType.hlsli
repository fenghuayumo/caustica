#ifndef __DEBUG_VIEW_TYPE_HLSLI__
#define __DEBUG_VIEW_TYPE_HLSLI__

// when editing don't forget to edit the UI in EditorUI.cpp (...ImGui::Combo( "Debug view"...)
enum class DebugViewType : int
{
    Disabled,
    DominantStablePlaneIndex,

    StablePlane_VirtualRayLength,
    StablePlane_MotionVectors,
    StablePlane_Normals,
    StablePlane_Roughness,
    StablePlane_SpecAvg,
    StablePlane_DiffBSDFEstimate,
    StablePlane_DiffRadiance,
    StablePlane_SpecBSDFEstimate,
    StablePlane_SpecRadiance,
    StablePlane_RelaxedDisocclusion,
    StablePlane_DiffRadianceDenoised,
    StablePlane_SpecRadianceDenoised,
    StablePlane_CombinedRadianceDenoised,
    StablePlane_ViewZ,
    StablePlane_Throughput,
    StablePlane_DenoiserValidation,

    StableRadiance,

    DenoiserGuide_Depth,
    DenoiserGuide_Roughness,
    DenoiserGuide_Albedo,
    DenoiserGuide_SpecAlbedo,
    DenoiserGuide_Normal,
    DenoiserGuide_MotionVectors,
    DenoiserGuide_SpecMotionVectors,
    DenoiserGuide_SpecHitT,
    DenoiserGuide_LayerWeights,
    DenoiserGuide_PrimaryLayer,

    FirstHit_Barycentrics,
    FirstHit_FaceNormal,
    FirstHit_GeometryNormal,
    FirstHit_ShadingNormal,
    FirstHit_ShadingTangent,
    FirstHit_ShadingBitangent,
    FirstHit_FrontFacing,
    FirstHit_ThinSurface,
    FirstHit_Diffuse,
    FirstHit_Specular,
    FirstHit_Roughness,
    FirstHit_Metallic,
    FirstHit_ShaderID,
    FirstHit_MaterialID,

    VBufferMotionVectors,
    VBufferDepth,

    SecondarySurfacePosition,
    SecondarySurfaceRadiance,
    ReSTIRGIOutput,

    ReSTIRDIInitialOutput,
    ReSTIRDITemporalOutput,
    ReSTIRDISpatialOutput,
    ReSTIRDIFinalOutput,
    ReSTIRDIFinalContribution,
    ReGIRIndirectOutput,

    MaxCount
};

#endif // __DEBUG_VIEW_TYPE_HLSLI__
