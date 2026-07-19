#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
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
#include <cstdint>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

void EditorUI::BuildOpacityMicroMapsPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Opacity Micro-Maps", ImGuiTreeNodeFlags_DefaultOpen))
        {
            UI_SCOPED_INDENT(layout.indent);

            if (auto& opacityMicromapBuilder = caustica::editor::requireWorldRenderer(m_sceneEditor).lightingPasses().opacityMaps(); opacityMicromapBuilder)
            {
                if (const auto scene = caustica::activeScene(*m_sceneEditor.app()))
                {
                    // Editor is outside beginGpuReadFrame; use the last published slot.
                    const uint32_t publishedFrame = scene->latestPublishedRenderFrameIndex();
                    if (publishedFrame != UINT32_MAX)
                        opacityMicromapBuilder->debugGUI(
                            layout.indent, scene->getRenderDataForFrame(publishedFrame));
                }
            }
            else
                ImGui::Text("<Opacity Micro-Maps not supported on the current device>");
        }


}

void EditorUI::BuildAccelerationStructurePanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Acceleration Structure", ImGuiTreeNodeFlags_DefaultOpen))
        {
            UI_SCOPED_INDENT(layout.indent);

            {
                if (ImGui::Checkbox("Force Opaque", &m_settings.AS.ForceOpaque))
                {
                    m_settings.ResetAccumulation = true;
                }

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Will set the instance flag ForceOpaque on all instances");
            }

            ImGui::Separator();
            ImGui::Text("settings below require AS rebuild");

            {
                if (ImGui::Checkbox("Exclude Transmissive", &m_settings.AS.ExcludeTransmissive))
                {
                    m_runtime.Invalidation.AccelerationStructRebuildRequested = true;
                }

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Will exclude all transmissive geometries from the BVH");
            }
        }


}

void EditorUI::BuildPostProcessPanel(const PanelLayout& layout)
{
    if (!m_editorUI.ShowPostProcess)
        return;

    if (!ImGui::Begin("Post Process", &m_editorUI.ShowPostProcess))
    {
        ImGui::End();
        return;
    }

    // Keep PushID/PopID inside the window and before End(); RAII after End()
    // trips ImGui's "Missing PopID()" assert for docked panels.
    ImGui::PushID("PostProcessPanel");

    if (ImGui::CollapsingHeader("Early (HDR)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
        SettingsCheckbox("Test Pass", &m_settings.PostProcessTestPassHDR);

        if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
            SettingsCheckbox("Enabled##Bloom", &m_settings.EnableBloom);
            SettingsSliderFloat("Width", &m_settings.BloomRadius, 0.f, 64.f, "%.1f px");
            SettingsSliderFloat("Intensity", &m_settings.BloomIntensity, 0.f, 0.1f, "%.4f");
        }
    }

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
        SettingsCheckbox("Enabled##ToneMapping", &m_settings.EnableToneMapping);

        const std::string currentOperator =
            tonemapOperatorToString.at(m_settings.ToneMappingParams.toneMapOperator);
        if (SettingsBeginCombo("Operator", currentOperator.c_str()))
        {
            for (auto it = tonemapOperatorToString.begin(); it != tonemapOperatorToString.end(); ++it)
            {
                const bool is_selected = it->first == m_settings.ToneMappingParams.toneMapOperator;
                if (ImGui::Selectable(it->second.c_str(), is_selected))
                    m_settings.ToneMappingParams.toneMapOperator = it->first;
            }
            SettingsEndCombo();
        }

        SettingsCategoryHeader("Exposure");
        SettingsCheckbox("Auto Exposure", &m_settings.ToneMappingParams.autoExposure);

        if (m_settings.ToneMappingParams.autoExposure)
        {
            SettingsInputFloat("Minimum EV", &m_settings.ToneMappingParams.exposureValueMin);
            m_settings.ToneMappingParams.exposureValueMin = dm::min(
                m_settings.ToneMappingParams.exposureValueMax,
                m_settings.ToneMappingParams.exposureValueMin);
            SettingsInputFloat("Maximum EV", &m_settings.ToneMappingParams.exposureValueMax);
            m_settings.ToneMappingParams.exposureValueMax = dm::max(
                m_settings.ToneMappingParams.exposureValueMin,
                m_settings.ToneMappingParams.exposureValueMax);
        }

        const std::string currentMode =
            ExposureModeToString.at(m_settings.ToneMappingParams.exposureMode);
        if (SettingsBeginCombo("Exposure Mode", currentMode.c_str()))
        {
            for (auto it = ExposureModeToString.begin(); it != ExposureModeToString.end(); ++it)
            {
                const bool is_selected = it->first == m_settings.ToneMappingParams.exposureMode;
                if (ImGui::Selectable(it->second.c_str(), is_selected))
                    m_settings.ToneMappingParams.exposureMode = it->first;
            }
            SettingsEndCombo();
        }

        SettingsInputFloat("Compensation", &m_settings.ToneMappingParams.exposureCompensation);
        m_settings.ToneMappingParams.exposureCompensation =
            dm::clamp(m_settings.ToneMappingParams.exposureCompensation, -12.0f, 12.0f);

        SettingsInputFloat("Exposure Value", &m_settings.ToneMappingParams.exposureValue);
        m_settings.ToneMappingParams.exposureValue = dm::clamp(
            m_settings.ToneMappingParams.exposureValue,
            dm::log2f(0.1f * 0.1f * 0.1f),
            dm::log2f(100000.f * 100.f * 100.f));

        SettingsInputFloat("Film Speed", &m_settings.ToneMappingParams.filmSpeed);
        m_settings.ToneMappingParams.filmSpeed =
            dm::clamp(m_settings.ToneMappingParams.filmSpeed, 1.0f, 6400.0f);

        SettingsInputFloat("F-Number", &m_settings.ToneMappingParams.fNumber);
        m_settings.ToneMappingParams.fNumber =
            dm::clamp(m_settings.ToneMappingParams.fNumber, 0.1f, 100.0f);

        SettingsInputFloat("Shutter", &m_settings.ToneMappingParams.shutter);
        m_settings.ToneMappingParams.shutter =
            dm::clamp(m_settings.ToneMappingParams.shutter, 0.1f, 10000.0f);

        SettingsCategoryHeader("White Balance");
        SettingsCheckbox("Enabled##WhiteBalance", &m_settings.ToneMappingParams.whiteBalance);

        SettingsInputFloat("White Point", &m_settings.ToneMappingParams.whitePoint);
        m_settings.ToneMappingParams.whitePoint =
            dm::clamp(m_settings.ToneMappingParams.whitePoint, 1905.0f, 25000.0f);

        SettingsInputFloat("Max Luminance", &m_settings.ToneMappingParams.whiteMaxLuminance);
        m_settings.ToneMappingParams.whiteMaxLuminance =
            dm::clamp(m_settings.ToneMappingParams.whiteMaxLuminance, 0.1f, FLT_MAX);

        SettingsInputFloat("White Scale", &m_settings.ToneMappingParams.whiteScale);
        m_settings.ToneMappingParams.whiteScale =
            dm::clamp(m_settings.ToneMappingParams.whiteScale, 0.f, 100.f);

        SettingsCheckbox("Clamp", &m_settings.ToneMappingParams.clamped);
    }

    if (ImGui::CollapsingHeader("Late (LDR)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
        SettingsCheckbox("Edge Detection", &m_settings.PostProcessEdgeDetection);
        SettingsSliderFloat(
            "Edge Threshold", &m_settings.PostProcessEdgeDetectionThreshold, 0.0f, 1.0f);
    }

    ImGui::PopID();
    ImGui::End();
}

void EditorUI::BuildDebuggingPanel(const PanelLayout& layout)
{
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.5, 1.0f));
        bool debuggingIsOpen = ImGui::CollapsingHeader("Debugging", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(1);
        if (debuggingIsOpen)
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );

            if (ImGui::CollapsingHeader("Debug switches"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                if (m_settings.RealtimeMode)
                {
                    ImGui::Checkbox("Freeze realtime noise seed", &m_settings.DbgFreezeRealtimeNoiseSeed);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Freeze global noise seed will not change per frame. Useful for \ndebugging transient issues hidden by noise, or for before/after comparison");
                }
                ImGui::Checkbox("Disable SER path termination hint", &m_settings.DbgDisableSERTerminationHint);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disable SER ReorderThread getting receive additional hint about path termination.");

                ImGui::Checkbox("Discard path (non-NEE) lighting", &m_settings.DbgDiscardNonNEELighting);
                ImGui::Checkbox("Discard NEE lighting", &m_settings.DbgDiscardNEELighting);
            }


#if ENABLE_DEBUG_VIZUALISATIONS
            if (ImGui::Combo("Debug view", (int*)&m_settings.DebugView,
                "Disabled\0"
                "DominantStablePlaneIndex\0StablePlane_VirtualRayLength\0StablePlane_MotionVectors\0"
                "StablePlane_Normals\0StablePlane_Roughness\0StablePlane_SpecAvg\0StablePlane_DiffBSDFEstimate\0StablePlane_DiffRadiance\0StablePlane_SpecBSDFEstimate\0StablePlane_SpecRadiance\0"
                "StablePlane_RelaxedDisocclusion\0StablePlane_DiffRadianceDenoised\0StablePlane_SpecRadianceDenoised\0StablePlane_CombinedRadianceDenoised\0StablePlane_ViewZ\0StablePlane_Throughput\0StablePlane_DenoiserValidation\0"
                "StableRadiance\0"
                "DenoiserGuide_Depth\0" "DenoiserGuide_Roughness\0" "DenoiserGuide_Albedo\0" "DenoiserGuide_SpecAlbedo\0"
                "DenoiserGuide_Normal\0" "DenoiserGuide_MotionVectors\0" "DenoiserGuide_SpecMotionVectors\0" "DenoiserGuide_SpecHitT\0" "DenoiserGuide_LayerWeights\0" "DenoiserGuide_PrimaryLayer\0"
                "FirstHit_Barycentrics\0FirstHit_FaceNormal\0FirstHit_GeometryNormal\0FirstHit_ShadingNormal\0FirstHit_ShadingTangent\0FirstHit_ShadingBitangent\0FirstHit_FrontFacing\0FirstHit_ThinSurface\0"
                "FirstHit_Diffuse\0FirstHit_Specular\0FirstHit_Roughness\0FirstHit_Metallic\0"
                "FirstHit_ShaderID\0FirstHit_MaterialID\0"
                "VBufferMotionVectors\0VBufferDepth\0"
                "SecondarySurfacePosition\0SecondarySurfaceRadiance\0ReSTIRGIOutput\0"
                "ReSTIRDIInitialOutput\0ReSTIRDITemporalOutput\0ReSTIRDISpatialOutput\0ReSTIRDIFinalOutput\0ReSTIRDIFinalContribution\0"
                "ReGIRIndirectOutput\0"
                "\0\0"))
                m_settings.ResetAccumulation = true;
            m_settings.DebugView = dm::clamp(m_settings.DebugView, (DebugViewType)0, DebugViewType::MaxCount);

            if (m_settings.DebugView >= DebugViewType::StablePlane_VirtualRayLength && m_settings.DebugView <= DebugViewType::StablePlane_DenoiserValidation)
            {
                m_settings.DebugViewStablePlaneIndex = dm::clamp(m_settings.DebugViewStablePlaneIndex, -1, (int)m_settings.StablePlanesActiveCount - 1);
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                float3 spcolor = (m_settings.DebugViewStablePlaneIndex >= 0) ? (StablePlaneDebugVizColor(m_settings.DebugViewStablePlaneIndex)) : (float3(1, 1, 0)); spcolor = spcolor * 0.7f + float3(0.2f, 0.2f, 0.2f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(spcolor.x, spcolor.y, spcolor.z, 1.0f));
                ImGui::InputInt("Stable Plane index", &m_settings.DebugViewStablePlaneIndex);
                ImGui::PopStyleColor(1);
                m_settings.DebugViewStablePlaneIndex = dm::clamp(m_settings.DebugViewStablePlaneIndex, -1, (int)m_settings.StablePlanesActiveCount - 1);
            }

            const DebugFeedbackStruct& feedback = caustica::feedbackData(*m_sceneEditor.app());
            // Display/window pixels; WorldRenderer maps to renderSize after DLSS.
            if (ImGui::InputInt2("Debug pixel", (int*)&m_settings.DebugPixel.x))
                m_sceneEditor.renderRuntimeState().Picking.requestMaterialPick();

            ImGui::Checkbox("Continuous feedback", &m_settings.ContinuousDebugFeedback);

            ImGui::Checkbox("Show debug lines", &m_settings.ShowDebugLines);

            if (ImGui::Checkbox("Show inspector", &m_editorUI.ShowInspector) && m_editorUI.ShowInspector)
            {
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
                m_editorUI.ShowDeltaTree = false;
#endif
            }

            if (ImGui::Checkbox("Show material editor", &m_editorUI.ShowMaterialEditor) && m_editorUI.ShowMaterialEditor)
            {
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
                m_editorUI.ShowDeltaTree = false;
#endif
            }

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
            if (!m_settings.ActualUseStablePlanes())
            {
                ImGui::Text("Enable Stable Planes for delta tree viz!");
                m_editorUI.ShowDeltaTree = false;
            }
            else
            {
                if (ImGui::Checkbox("Show delta tree window", &m_editorUI.ShowDeltaTree) && m_editorUI.ShowDeltaTree)
                {
                    m_editorUI.ShowInspector = false; // no space for both
                    m_sceneEditor.renderRuntimeState().Picking.requestMaterialPick();
                }
            }
#else
            ImGui::Text("Delta tree debug viz disabled; to enable set ENABLE_DEBUG_DELTA_TREE_VIZUALISATION to 1");
#endif
            ImGui::Separator();

            // Slots are reset to (-1,-1,-1,-1) when unused. Only list slots that shaders
            // actually wrote, and only while feedback is being sampled — avoids a noisy
            // idle wall of -1s when Continuous feedback is off.
            if (m_settings.ContinuousDebugFeedback)
            {
                int written = 0;
                for (int i = 0; i < MAX_DEBUG_PRINT_SLOTS; i++)
                {
                    const auto& v = feedback.debugPrint[i];
                    if (v.x == -1.f && v.y == -1.f && v.z == -1.f && v.w == -1.f)
                        continue;
                    ImGui::Text("debugPrint %d: %f, %f, %f, %f", i, v.x, v.y, v.z, v.w);
                    ++written;
                }
                if (written == 0)
                    ImGui::TextDisabled("debugPrint: idle (no shader Print for this pixel)");
            }
            ImGui::Text("Debug line count: %d", feedback.lineVertexCount / 2);
            ImGui::InputFloat("Debug Line Scale", &m_settings.DebugLineScale);
#else
            ImGui::TextWrapped("Debug visualization disabled; to enable set ENABLE_DEBUG_VIZUALISATIONS to 1");
#endif 

            if (m_sceneEditor.zoomTool() != nullptr && ImGui::CollapsingHeader("Zoom Tool"))
                m_sceneEditor.zoomTool()->debugGUI(layout.indent);
        }


}

void EditorUI::BuildQuickToneMappingBar(const PanelLayout& layout)
{
        {
            // quick tonemapping settings
            ImGui::PushItemWidth(layout.defItemWidth * 0.7f);
            const char* tooltipInfo = "Detailed exposure settings are in Tone Mapping section";
            ImGui::PushID("QS");
            ImGui::Checkbox("AutoExposure", &m_settings.ToneMappingParams.autoExposure); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltipInfo);
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::SliderFloat("Brightness", &m_settings.ToneMappingParams.exposureCompensation, -18.0f, 8.0f, "%.2f");  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltipInfo);
            ImGui::SameLine();
            if (ImGui::Button("0"))
                m_settings.ToneMappingParams.exposureCompensation = 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltipInfo);
            ImGui::PopID();
            ImGui::PopItemWidth();
        }

}


} // namespace caustica::editor

