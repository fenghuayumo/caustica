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

void EditorUI::BuildOpacityMicroMapsPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Opacity Micro-Maps"))
        {
            UI_SCOPED_INDENT(layout.indent);

            if (auto& opacityMicromapBuilder = m_sceneEditor.GetLightingPasses().opacityMaps(); opacityMicromapBuilder)
            {
                opacityMicromapBuilder->DebugGUI(layout.indent, *m_sceneEditor.GetScene());
            }
            else
                ImGui::Text("<Opacity Micro-Maps not supported on the current device>");
        }


}

void EditorUI::BuildAccelerationStructurePanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Acceleration Structure"))
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
            ImGui::Text("Settings below require AS rebuild");

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
        if (ImGui::CollapsingHeader("Post-process"))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );

            if (ImGui::CollapsingHeader("Early (HDR) post-process"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                ImGui::Checkbox("PostProcessTestPass", &m_settings.PostProcessTestPassHDR );
                
                ImGui::Separator();

                if (ImGui::CollapsingHeader("Bloom"))
                {
                    ImGui::Checkbox("Enable Bloom", &m_settings.EnableBloom);
                    ImGui::SliderFloat("Bloom Width (Pixels)", &m_settings.BloomRadius, 0.f, 64.f);
                    ImGui::SliderFloat("Bloom Intensity", &m_settings.BloomIntensity, 0.f, 0.1f);
                }
            }

            if (ImGui::CollapsingHeader("Tone Mapping"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                ImGui::Checkbox("Enable", &m_settings.EnableToneMapping);

                const std::string currentOperator = tonemapOperatorToString.at(m_settings.ToneMappingParams.toneMapOperator);
                if (ImGui::BeginCombo("Operator", currentOperator.c_str()))
                {
                    for (auto it = tonemapOperatorToString.begin(); it != tonemapOperatorToString.end(); it++)
                    {
                        bool is_selected = it->first == m_settings.ToneMappingParams.toneMapOperator;
                        if (ImGui::Selectable(it->second.c_str(), is_selected))
                            m_settings.ToneMappingParams.toneMapOperator = it->first;
                    }
                    ImGui::EndCombo();
                }

                ImGui::Checkbox("Auto Exposure", &m_settings.ToneMappingParams.autoExposure);

                if (m_settings.ToneMappingParams.autoExposure)
                {
                    ImGui::InputFloat("Auto Exposure Min", &m_settings.ToneMappingParams.exposureValueMin);
                    m_settings.ToneMappingParams.exposureValueMin = dm::min(m_settings.ToneMappingParams.exposureValueMax, m_settings.ToneMappingParams.exposureValueMin);
                    ImGui::InputFloat("Auto Exposure Max", &m_settings.ToneMappingParams.exposureValueMax);
                    m_settings.ToneMappingParams.exposureValueMax = dm::max(m_settings.ToneMappingParams.exposureValueMin, m_settings.ToneMappingParams.exposureValueMax);
                }

                const std::string currentMode = ExposureModeToString.at(m_settings.ToneMappingParams.exposureMode);
                if (ImGui::BeginCombo("Exposure Mode", currentMode.c_str()))
                {
                    for (auto it = ExposureModeToString.begin(); it != ExposureModeToString.end(); it++)
                    {
                        bool is_selected = it->first == m_settings.ToneMappingParams.exposureMode;
                        if (ImGui::Selectable(it->second.c_str(), is_selected))
                            m_settings.ToneMappingParams.exposureMode = it->first;
                    }
                    ImGui::EndCombo();
                }

                ImGui::InputFloat("Exposure Compensation", &m_settings.ToneMappingParams.exposureCompensation);
                m_settings.ToneMappingParams.exposureCompensation = dm::clamp(m_settings.ToneMappingParams.exposureCompensation, -12.0f, 12.0f);

                ImGui::InputFloat("Exposure Value", &m_settings.ToneMappingParams.exposureValue);
                m_settings.ToneMappingParams.exposureValue = dm::clamp(m_settings.ToneMappingParams.exposureValue, dm::log2f(0.1f * 0.1f * 0.1f), dm::log2f(100000.f * 100.f * 100.f));

                ImGui::InputFloat("Film Speed", &m_settings.ToneMappingParams.filmSpeed);
                m_settings.ToneMappingParams.filmSpeed = dm::clamp(m_settings.ToneMappingParams.filmSpeed, 1.0f, 6400.0f);

                ImGui::InputFloat("fNumber", &m_settings.ToneMappingParams.fNumber);
                m_settings.ToneMappingParams.fNumber = dm::clamp(m_settings.ToneMappingParams.fNumber, 0.1f, 100.0f);

                ImGui::InputFloat("Shutter", &m_settings.ToneMappingParams.shutter);
                m_settings.ToneMappingParams.shutter = dm::clamp(m_settings.ToneMappingParams.shutter, 0.1f, 10000.0f);

                ImGui::Checkbox("Enable White Balance", &m_settings.ToneMappingParams.whiteBalance);

                ImGui::InputFloat("White Point", &m_settings.ToneMappingParams.whitePoint);
                m_settings.ToneMappingParams.whitePoint = dm::clamp(m_settings.ToneMappingParams.whitePoint, 1905.0f, 25000.0f);

                ImGui::InputFloat("White Max Luminance", &m_settings.ToneMappingParams.whiteMaxLuminance);
                m_settings.ToneMappingParams.whiteMaxLuminance = dm::clamp(m_settings.ToneMappingParams.whiteMaxLuminance, 0.1f, FLT_MAX);

                ImGui::InputFloat("White Scale", &m_settings.ToneMappingParams.whiteScale);
                m_settings.ToneMappingParams.whiteScale = dm::clamp(m_settings.ToneMappingParams.whiteScale, 0.f, 100.f);

                ImGui::Checkbox("Enable Clamp", &m_settings.ToneMappingParams.clamped);
            }

            if (ImGui::CollapsingHeader("Late (LDR) post-process"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                ImGui::Checkbox("EdgeDetection", &m_settings.PostProcessEdgeDetection);
                ImGui::SliderFloat("EdgeDetectionThreshold", &m_settings.PostProcessEdgeDetectionThreshold, 0.0f, 1.0f );
                ImGui::Separator();
            }
        }


}

void EditorUI::BuildDebuggingPanel(const PanelLayout& layout)
{
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.5, 1.0f));
        bool debuggingIsOpen = ImGui::CollapsingHeader("Debugging"); //, ImGuiTreeNodeFlags_DefaultOpen ) )
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

            const DebugFeedbackStruct& feedback = m_sceneEditor.GetFeedbackData();
            if (ImGui::InputInt2("Debug pixel", (int*)&m_settings.DebugPixel.x))
                m_sceneEditor.GetRenderRuntimeState().Picking.requestMaterialPick();

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
                    m_sceneEditor.GetRenderRuntimeState().Picking.requestMaterialPick();
                }
            }
#else
            ImGui::Text("Delta tree debug viz disabled; to enable set ENABLE_DEBUG_DELTA_TREE_VIZUALISATION to 1");
#endif
            ImGui::Separator();

            for (int i = 0; i < MAX_DEBUG_PRINT_SLOTS; i++)
                ImGui::Text("debugPrint %d: %f, %f, %f, %f", i, feedback.debugPrint[i].x, feedback.debugPrint[i].y, feedback.debugPrint[i].z, feedback.debugPrint[i].w);
            ImGui::Text("Debug line count: %d", feedback.lineVertexCount / 2);
            ImGui::InputFloat("Debug Line Scale", &m_settings.DebugLineScale);
#else
            ImGui::TextWrapped("Debug visualization disabled; to enable set ENABLE_DEBUG_VIZUALISATIONS to 1");
#endif 

            if (m_sceneEditor.GetZoomTool() != nullptr && ImGui::CollapsingHeader("Zoom Tool"))
                m_sceneEditor.GetZoomTool()->DebugGUI(layout.indent);
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

