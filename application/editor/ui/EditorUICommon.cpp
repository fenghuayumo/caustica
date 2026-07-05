#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorApplication.h"
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <game/GameScene.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

const ImVec4 warnColor = { 1,0.5f,0.5f,1 };
const ImVec4 categoryColor = { 0.5f,1.0f,0.7f,1 };

namespace
{
    bool IsMeshInstanceEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        auto* comp = ew.world().tryGet<caustica::scene::MeshInstanceComponent>(entity);
        return comp != nullptr && comp->mesh != nullptr;
    }

    bool IsGaussianSplatEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        auto* comp = ew.world().tryGet<caustica::scene::GaussianSplatComponent>(entity);
        return comp != nullptr && comp->splat != nullptr;
    }

    bool IsInspectableEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        return IsMeshInstanceEntity(ew, entity) || IsGaussianSplatEntity(ew, entity);
    }

    bool HasHierarchyEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        if (IsInspectableEntity(ew, entity)) return true;
        for (ecs::Entity child : ew.getEntityChildren(entity))
            if (HasHierarchyEntity(ew, child)) return true;
        return false;
    }

    float WrapDegrees(float degrees)
    {
        degrees = std::fmod(degrees, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees;
    }
}

int ResolveGaussianSplatShadowMode(const EditorUIData& ui)
{
        if (!ui.session.settings.GaussianSplatShadows && ui.session.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = ui.session.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : ui.session.settings.GaussianSplatShadowsMode;
        return dm::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT);
    }

    bool GaussianSplatModeCombo(EditorUIData& ui)
    {
        int renderingMode = ResolveGaussianSplatShadowMode(ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED ? 1 : 0;
        if (!ImGui::Combo("Rendering Mode", &renderingMode, "Raster 3DGS (VS)\0Hybrid 3DGS + 3DGRT\0\0"))
            return false;

        if (renderingMode == 1)
        {
            ui.session.settings.GaussianSplatShadows = true;
            if (ui.session.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
                ui.session.settings.GaussianSplatShadowsMode = GAUSSIAN_SPLAT_SHADOWS_HARD;
        }
        else
        {
            ui.session.settings.GaussianSplatShadows = false;
            ui.session.settings.GaussianSplatShadowsMode = GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        }
        ui.session.runtime.Invalidation.AccelerationStructRebuildRequested = true;
        ui.session.settings.ResetAccumulation = true;
        return true;
    }

    bool GaussianSplatShadowsModeCombo(EditorUIData& ui)
    {
        const bool wasEnabled = ResolveGaussianSplatShadowMode(ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        int shadowMode = ResolveGaussianSplatShadowMode(ui);

        ui.session.settings.GaussianSplatShadowsMode = shadowMode;
        ui.session.settings.GaussianSplatShadows = shadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        if (!ImGui::Combo("Shadows Mode", &shadowMode, "Shadows off\0Hard shadows\0Soft shadows\0\0"))
            return false;

        shadowMode = dm::clamp(shadowMode, GAUSSIAN_SPLAT_SHADOWS_DISABLED, GAUSSIAN_SPLAT_SHADOWS_SOFT);
        ui.session.settings.GaussianSplatShadowsMode = shadowMode;
        ui.session.settings.GaussianSplatShadows = shadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        if (wasEnabled != ui.session.settings.GaussianSplatShadows)
            ui.session.runtime.Invalidation.AccelerationStructRebuildRequested = true;
        ui.session.settings.ResetAccumulation = true;
        return true;
    }

    bool GaussianSplatSortingCombo(EditorUIData& ui)
    {
        const bool changed = ImGui::Combo("Sorting Method", &ui.session.settings.GaussianSplatSortingMode, "GPU sort\0Stochastic Splats\0\0");
        ui.session.settings.GaussianSplatSortingMode = dm::clamp(ui.session.settings.GaussianSplatSortingMode, 0, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("GPU sort uses the existing radix-sort path. Stochastic splats uses stable randomized order plus stochastic opacity accept/reject.");
        return changed;
    }

    bool GaussianSplatFormatCombo(const char* label, int* value)
    {
        const bool changed = ImGui::Combo(label, value, "Float32\0Float16\0Uint8\0\0");
        *value = dm::clamp(*value, 0, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Storage format used by the 3DGS raster color/alpha and SH buffers in VRAM.");
        return changed;
    }

    bool GaussianSplatFTBCombo(EditorUIData& ui)
    {
        const bool changed = ImGui::Combo("FTB Sync Mode", &ui.session.settings.GaussianSplatFTBSyncMode, "Disabled (fast)\0Interlock\0\0");
        ui.session.settings.GaussianSplatFTBSyncMode = dm::clamp(ui.session.settings.GaussianSplatFTBSyncMode, 0, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Front-to-back depth synchronization mode. The current RTXPT overlay path does not write a 3DGS depth iso buffer yet.");
        return changed;
    }

    bool GaussianSplatRtxKernelDegreeCombo(EditorUIData& ui)
    {
        const bool changed = ImGui::Combo("Kernel degree", &ui.session.settings.GaussianSplatRtxKernelDegree,
            "0 (Linear)\0"
            "1 (Laplacian)\0"
            "2 (Quadratic)\0"
            "3 (Cubic)\0"
            "4 (Tesseractic)\0"
            "5 (Quintic)\0\0");
        ui.session.settings.GaussianSplatRtxKernelDegree = dm::clamp(ui.session.settings.GaussianSplatRtxKernelDegree, 0, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Kernel degree for the 3DGRT particle intersection shape. Changing it rebuilds Gaussian BLAS proxies.");
        return changed;
    }

    bool GaussianSplatRtxParticleFormatCombo(EditorUIData& ui)
    {
        int particleFormat = ui.session.settings.GaussianSplatUseAABBs ? 1 : 0;
        const bool changed = ImGui::Combo("Particles format", &particleFormat, "Icosahedron\0AABB + parametric\0\0");
        if (changed)
        {
            ui.session.settings.GaussianSplatUseAABBs = particleFormat == 1;
            if (ui.session.settings.GaussianSplatUseAABBs)
                ui.session.settings.GaussianSplatUseTLASInstances = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shortcut for the 3DGS RTX acceleration proxy format. AABB + parametric forces TLAS instances.");
        return changed;
    }

void BuildHierarchyNodeUI(EditorUIData& ui, caustica::Scene& scene, ecs::Entity entity)
    {
        auto* ew = scene.GetEntityWorld();
        if (!ew || entity == ecs::NullEntity) return;
        if (!HasHierarchyEntity(*ew, entity)) return;

        const bool isMeshEntity = IsMeshInstanceEntity(*ew, entity);
        const bool isGaussianSplatEntity = IsGaussianSplatEntity(*ew, entity);
        const auto& children = ew->getEntityChildren(entity);

        bool hasVisibleChildren = false;
        for (ecs::Entity child : children)
        {
            if (HasHierarchyEntity(*ew, child))
            {
                hasVisibleChildren = true;
                break;
            }
        }

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasVisibleChildren)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (ui.editor.SelectedEntity == entity)
            flags |= ImGuiTreeNodeFlags_Selected;

        std::string nodeName = ew->getEntityName(entity);
        if (nodeName.empty()) nodeName = "<unnamed>";
        std::string label = isMeshEntity ? "[Mesh] " + nodeName : (isGaussianSplatEntity ? "[3DGS] " + nodeName : "[Group] " + nodeName);
        if (isMeshEntity)
        {
            auto* comp = ew->world().tryGet<caustica::scene::MeshInstanceComponent>(entity);
            if (comp && comp->mesh)
                label += "  (" + comp->mesh->name + ")";
        }
        else if (isGaussianSplatEntity)
        {
            auto* comp = ew->world().tryGet<caustica::scene::GaussianSplatComponent>(entity);
            if (comp && comp->splat && comp->splat->loadedSplatCount > 0)
                label += "  (" + std::to_string(comp->splat->loadedSplatCount) + " splats)";
        }

        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), flags, "%s", label.c_str());

        if (IsInspectableEntity(*ew, entity) && ImGui::IsItemClicked())
        {
            ui.editor.SelectedEntity = entity;
            ui.editor.SelectedGaussianSplat = false;
        }

        if (isMeshEntity && ImGui::IsItemHovered())
            ImGui::SetTooltip("Mesh instance. Click to open it in Inspector.");
        if (isGaussianSplatEntity && ImGui::IsItemHovered())
            ImGui::SetTooltip("3D Gaussian Splat scene object. Click to open it in Inspector.");

        if (open && hasVisibleChildren)
        {
            for (ecs::Entity child : children)
                BuildHierarchyNodeUI(ui, scene, child);
            ImGui::TreePop();
        }
    }

dm::float3 QuaternionToEulerDegreesXYZ(const dm::dquat& rotation)
    {
        constexpr float rad2deg = 180.0f / 3.14159265f;
        const dm::double3x3 m = rotation.toMatrix();

        const double y = std::asin(dm::clamp(-m.m_data[2], -1.0, 1.0));
        const double cy = std::cos(y);

        double x = 0.0;
        double z = 0.0;
        if (std::abs(cy) > 1e-8)
        {
            x = std::atan2(m.m_data[5], m.m_data[8]);
            z = std::atan2(m.m_data[1], m.m_data[0]);
        }
        else
        {
            x = std::atan2(-m.m_data[7], m.m_data[4]);
        }

        return dm::float3(
            WrapDegrees(float(x) * rad2deg),
            WrapDegrees(float(y) * rad2deg),
            WrapDegrees(float(z) * rad2deg));
    }

    bool SameRotation(const dm::dquat& a, const dm::dquat& b)
    {
        const double lenA = dm::length(a);
        const double lenB = dm::length(b);
        if (lenA <= 1e-12 || lenB <= 1e-12)
            return false;

        const double cosine = std::abs(dm::dot(a / lenA, b / lenB));
        return cosine > 0.999999999;
    }

const ::PerformancePreset s_performancePresets[kPerformancePresetCount] = {
    //                                    NEECand  NEEFull  NEEMIS  SPP  Bounce  DiffBnc   TexLOD  NestDiel  EnvMIP  SPActive  PrimRepl  Bloom    LDSampl    FflyTrhld    DLSS (on separate line due to macros)
    { "Ultra Performance",                3,       1,       1,      1,   10,     1,        0.0f,   1,        3,      2,        false,    false,   false,     0.01,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eUltraPerformance,
#endif
    },
    { "Performance",                      3,       1,       1,      1,   12,     1,       -0.5f,   1,        2,      3,        true,     true,    false,     0.05,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eMaxPerformance,
#endif
    },
    { "Balanced",                         5,       1,       1,      1,   18,     2,       -1.0f,   1,        2,      3,        true,     true,    true,      0.1,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eBalanced,
#endif
    },
    { "Quality",                          3,       2,       1,      1,   24,     3,       -1.5f,   1,        2,      3,        true,     true,    true,      0.2,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eMaxQuality,
#endif
    },
    { "Ultra Quality",                    3,       2,       0,      1,   48,     3,       -1.5f,   2,        1,      3,        true,     true,    true,      1.0,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eDLAA,
#endif
    },
};
bool MatchesPreset(const EditorUIData& ui, const ::PerformancePreset& p)
{
    return caustica::render::MatchesPerformancePreset(ui.session.settings, p);
}

void ApplyPreset(EditorUIData& ui, const ::PerformancePreset& p)
{
    caustica::render::ApplyPerformancePreset(ui.session.settings, p);
}

} // namespace caustica::editor

void PathTracerSettings::ApplyRTXDIRestirPreset()
{
    if (RTXDIRestirPreset == RTXDIRestirQualityPreset::Custom)
        return;

    const bool wasUsingCheckerboard = RTXDI.checkerboardMode != rtxdi::CheckerboardMode::Off;
    bool enableCheckerboardSampling = wasUsingCheckerboard;

    RTXDI.restirDI.resamplingMode = GetReSTIRDI_ResamplingMode();
    RTXDI.restirDI.initialSamplingParams = getReSTIRDIInitialSamplingParams();
    RTXDI.restirDI.temporalResamplingParams = getReSTIRDITemporalResamplingParams();
    RTXDI.restirDI.spatialResamplingParams = getReSTIRDISpatialResamplingParams();
    RTXDI.restirDI.shadingParams = getReSTIRDIShadingParams();

    RTXDI.restirGI.resamplingMode = GetReSTIRGI_ResamplingMode();
    RTXDI.restirGI.temporalResamplingParams = getReSTIRGITemporalResamplingParams();
    RTXDI.restirGI.spatialResamplingParams = getReSTIRGISpatialResamplingParams();
    RTXDI.restirGI.finalShadingParams = getReSTIRGIFinalShadingParams();

    switch (RTXDIRestirPreset)
    {
    case RTXDIRestirQualityPreset::Fast:
        enableCheckerboardSampling = true;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 4;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 0;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Off;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Off;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 6;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 30;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Basic;
        break;

    case RTXDIRestirQualityPreset::Medium:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 10;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Basic;
        break;

    case RTXDIRestirQualityPreset::Unbiased:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Ultra:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 16;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 20;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Reference:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::None;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::None;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Custom:
    default:
        break;
    }

    RTXDI.checkerboardMode = enableCheckerboardSampling ? rtxdi::CheckerboardMode::Black : rtxdi::CheckerboardMode::Off;
    ResetAccumulation = true;
    ResetRealtimeCaches |= wasUsingCheckerboard != enableCheckerboardSampling;
}

void PathTracerSettings::ApplyRTXDIRestirPTPreset()
{
    if (RTXDIRestirPTPreset == RTXDIRestirPTQualityPreset::Custom)
        return;

    RTXDI.restirPT.initialSamplingParams = getReSTIRPTInitialSamplingParams();
    RTXDI.restirPT.temporalResamplingParams = getReSTIRPTTemporalResamplingParams();
    RTXDI.restirPT.reconnectionParams = getReSTIRPTReconnectionParams();
    RTXDI.restirPT.hybridShiftParams = getReSTIRPTHybridShiftParams();
    RTXDI.restirPT.boilingFilterParams = getReSTIRPTBoilingFilterParams();
    RTXDI.restirPT.spatialResamplingParams = getReSTIRPTSpatialResamplingParams();

    switch (RTXDIRestirPTPreset)
    {
    case RTXDIRestirPTQualityPreset::Fast:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::Temporal;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Medium:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 4;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Ultra:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 4;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Custom:
    default:
        break;
    }

    RTXDI.restirPT.hybridShiftParams.maxBounceDepth = RTXDI.restirPT.initialSamplingParams.maxBounceDepth;
    RTXDI.restirPT.hybridShiftParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxRcVertexLength;
    RTXDI.restirPT.spatialResamplingParams.maxTemporalHistory = RTXDI.restirPT.temporalResamplingParams.maxHistoryLength;
    ResetAccumulation = true;
}

namespace caustica::editor
{

#if CAUSTICA_WITH_ANY_DLSS
SI::DLSSMode DLSSModeUI(SI::DLSSMode dlssModeCurrent)
{
    int current = -1;
    switch (dlssModeCurrent)
    {
    case SI::DLSSMode::eMaxPerformance:    current = 1; break;
    case SI::DLSSMode::eBalanced:          current = 2; break;
    case SI::DLSSMode::eMaxQuality:        current = 3; break;
    case SI::DLSSMode::eUltraPerformance:  current = 0; break;
    case SI::DLSSMode::eDLAA:              current = 4; break;
    default: assert(false); return SI::DLSSMode::eBalanced;
    }

    ImGui::Combo("DLSS Resolution Scale", (int*)&current, "UltraPerformance\0Performance\0Balanced\0Quality\0DLAA\0");

    switch (current)
    {
    case 0 : return SI::DLSSMode::eUltraPerformance;
    case 1 : return SI::DLSSMode::eMaxPerformance;
    case 2 : return SI::DLSSMode::eBalanced;
    case 3 : return SI::DLSSMode::eMaxQuality;
    case 4 : return SI::DLSSMode::eDLAA;
    default: assert(false); return SI::DLSSMode::eBalanced;

    }
    ImGui::Text("(DLSS setting also apply to Ray Reconstruction)");
}
#endif

std::string TrimTogglable(const std::string text)
{
    size_t tog = text.rfind("_togglable");
    if (tog != std::string::npos)
        return text.substr(0, tog);
    return text;
}
std::string TrimSkyDisplayName(std::string text)
{
    if (text == c_EnvMapSceneDefault)
        return "default";
    else if (text == c_EnvMapProcSky)
        return "procedural";
    else if (text == c_EnvMapProcSky_Morning)
        return "morning";
    else if (text == c_EnvMapProcSky_Midday)
        return "midday";
    else if (text == c_EnvMapProcSky_Evening)
        return "evening";
    else if (text == c_EnvMapProcSky_Dawn)
        return "dawn";
    else if (text == c_EnvMapProcSky_PitchBlack)
        return "pitch black";
    return "unknown";
}

bool TogglableNode::IsSelected() const
{
    if (!EntityWorld || Entity == ecs::NullEntity) return false;
    auto* comp = EntityWorld->world().tryGet<caustica::scene::LocalTransformComponent>(Entity);
    if (!comp) return false;
    return all(comp->translation == OriginalTranslation);
}

void TogglableNode::SetSelected(bool selected)
{
    if (!EntityWorld || Entity == ecs::NullEntity) return;
    if (selected)
        EntityWorld->setTranslation(Entity, OriginalTranslation);
    else
        EntityWorld->setTranslation(Entity, {-10000.0, -10000.0, -10000.0});
}

void UpdateTogglableNodes(std::vector<TogglableNode>& togglableNodes, caustica::scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    if (entity == ecs::NullEntity) return;

    auto addIfTogglable = [&](const std::string& token, ecs::Entity e) -> TogglableNode*
    {
        const size_t tokenLen = token.length();
        const std::string name = entityWorld.getEntityName(e);
        const size_t nameLen = name.length();
        if (nameLen > tokenLen && name.substr(nameLen - tokenLen) == token)
        {
            TogglableNode tn;
            tn.Entity = e;
            tn.EntityWorld = &entityWorld;
            tn.UIName = name.substr(0, nameLen - tokenLen);
            auto* comp = entityWorld.world().tryGet<caustica::scene::LocalTransformComponent>(e);
            tn.OriginalTranslation = comp ? comp->translation : dm::double3(0.0);
            togglableNodes.push_back(tn);
            return &togglableNodes.back();
        }
        return nullptr;
    };

    TogglableNode* justAdded = addIfTogglable("_togglable", entity);
    if (justAdded == nullptr)
    {
        justAdded = addIfTogglable("_togglable_off", entity);
        if (justAdded != nullptr)
            justAdded->SetSelected(false);
    }

    const auto& children = entityWorld.getEntityChildren(entity);
    for (int i = (int)children.size() - 1; i >= 0; i--)
        UpdateTogglableNodes(togglableNodes, entityWorld, children[i]);
}

} // namespace caustica::editor
