#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorApplication.h"
#include "common/ImGuiManager.h"

#include <render/Core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneGraph.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/Passes/Debug/Korgi.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <game/GameScene.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

void EditorUI::BuildStochasticTextureFilteringPanel(const PanelLayout& layout)
{
#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    if (ImGui::CollapsingHeader("Stochastic Texture Filtering"))
    {
        bool changed = false;
        changed = ImGui::Combo("Magnification Method", (int*)&m_ui.STFMagnificationMethod,
            "Default\0"
            "Quad2x2\0"
            "Fine2x2\0"
            "FineTemporal2x2\0"
            "FineAlu3x3\0"
            "FineLut3x3\0"
            "Fine4x4\0"
        );
        if (changed)
        {
            caustica::debug("Magnification Method ", static_cast<int>(m_ui.STFMagnificationMethod));
        }

        changed = ImGui::Combo("Filter Type", (int*)&m_ui.STFFilterMode,
            "Point\0"
            "Linear\0"
            "Cubic\0"
            "Gaussian\0"
        );
        if (changed)
        {
            caustica::debug("Filter Type ", static_cast<int>(m_ui.STFFilterMode));
        }

        ImGui::BeginDisabled(m_ui.STFFilterMode != StfFilterMode::Gaussian);
        ImGui::SliderFloat("Sigma", &m_ui.STFGaussianSigma, 0.f, 100.f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();   // m_ui.STFFilterMode
    }
#endif
}

void EditorUI::BuildDLSSReflexPanel(const PanelLayout& layout)
{
        if (m_ui.RealtimeMode && m_ui.RealtimeAA > 1 && ImGui::CollapsingHeader("DLSS & Reflex settings"))
        {
            ImGui::TextColored(categoryColor, "Anti-aliasing and super-resolution");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

#if CAUSTICA_WITH_ANY_DLSS
                if (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3)
                    m_ui.DLSSMode = DLSSModeUI(m_ui.DLSSMode);
    
                if (m_ui.RealtimeAA == 3)
                {
                    ImGui::SliderFloat("DLSS-RR micro jitter", &m_ui.DLSSRRMicroJitter, 0.0f, 1.0f);
					int presetIndex = 0;
					switch (m_ui.DLSRRPreset) {
						case SI::DLSSRRPreset::eDefault: presetIndex = 0; break;
						case SI::DLSSRRPreset::ePresetD: presetIndex = 1; break;
						case SI::DLSSRRPreset::ePresetE: presetIndex = 2; break;
					}
                    ImGui::Combo("DLSS-RR Preset", &presetIndex, "Default\0PresetD\0PresetE\0");
					const SI::DLSSRRPreset DLSSRR_PRESETS[] = { SI::DLSSRRPreset::eDefault, SI::DLSSRRPreset::ePresetD, SI::DLSSRRPreset::ePresetE };  // Maps combo index to enum value
					m_ui.DLSRRPreset = DLSSRR_PRESETS[presetIndex];
					m_ui.DLSRRPreset = clamp(m_ui.DLSRRPreset, SI::DLSSRRPreset::eDefault, SI::DLSSRRPreset::ePresetE);
                }
#endif
                ImGui::Combo("AA Camera Jitter", (int*)&m_ui.TemporalAntiAliasingJitter, "MSAA\0Halton\0R2\0White Noise\0");

            }

            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );

            if (ImGui::CollapsingHeader("Reflex", 0))
            {
    #if CAUSTICA_WITH_STREAMLINE
                ImGui::Text("Reflex LowLatency Supported: %s", m_ui.IsReflexSupported && m_ui.IsReflexLowLatencyAvailable ? "yes" : "no");
                if (m_ui.IsReflexSupported && m_ui.IsReflexLowLatencyAvailable)
                {
                    ImGui::Combo("Reflex Low Latency", (int*)&m_ui.ReflexMode, "Off\0On\0On + Boost\0");

                    bool useFrameCap = m_ui.ReflexCappedFps != 0;
                    if (ImGui::Checkbox("Reflex FPS Capping", &useFrameCap))
                    {
                        if (useFrameCap) { m_ui.FpsCap = 0; }
                    }
                    else if (m_ui.FpsCap != 0)
                    {
                        useFrameCap = false;
                        m_ui.ReflexCappedFps = 0;
                    }

                    if (useFrameCap)
                    {
                        if (m_ui.ReflexCappedFps == 0) { m_ui.ReflexCappedFps = 60; }
                        ImGui::SameLine();
                        ImGui::DragInt("##FPSReflexCap", &m_ui.ReflexCappedFps, 1.f, 20, 240);
                        m_ui.FpsCap = 0;
                    }
                    else
                    {
                        m_ui.ReflexCappedFps = 0;
                    }

                    ImGui::Checkbox("Show Stats Report", &m_ui.ReflexShowStats);
                    if (m_ui.ReflexShowStats)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        ImGui::Text(m_ui.ReflexStats.c_str());
                    }

                    if (!m_ui.RealtimeMode)
                        ImGui::TextColored(warnColor, "Note: Reflex is DISABLED in Reference PT mode");
                }
    #else
                ImGui::Text("Compiled without REFLEX enabled");
    #endif
            }

            if (ImGui::CollapsingHeader("DLSS-G", 0))
            {
    #if CAUSTICA_WITH_STREAMLINE
                ImGui::Text("DLSS-G Supported: %s", m_ui.IsDLSSFGSupported ? "yes" : "no");
                if (m_ui.IsDLSSFGSupported)
                {

                    if (m_ui.ReflexMode == caustica::StreamlineInterface::ReflexMode::eOff)
                        ImGui::Text("Please note: Reflex is currently off and will be\nautomatically enabled if DLSS-FG is enabled");

                    DLSSFGSelectorUI();
                }
    #else
                ImGui::Text("Compiled without DLSS-G enabled");
    #endif
            }
        }


}

void EditorUI::BuildTAAPanel(const PanelLayout& layout)
{
        if( m_ui.RealtimeMode && m_ui.RealtimeAA == 1 && ImGui::CollapsingHeader("TAA settings") )
        {
            ImGui::Checkbox("TAA History Clamping", &m_ui.TemporalAntiAliasingParams.enableHistoryClamping);
            ImGui::SliderFloat("TAA New Frame Weight", &m_ui.TemporalAntiAliasingParams.newFrameWeight, 0.001f, 1.0f);
            ImGui::Checkbox("TAA Use Clamp Relax", &m_ui.TemporalAntiAliasingParams.useHistoryClampRelax);
            ImGui::Combo("AA Camera Jitter", (int*)&m_ui.TemporalAntiAliasingJitter, "MSAA\0Halton\0R2\0White Noise\0");
        }


}

void EditorUI::BuildRTXDIPanel(const PanelLayout& layout)
{
        if ( (m_ui.ActualUseReSTIRDI() || m_ui.ActualUseReSTIRGI() || m_ui.ActualUseReSTIRPT()) && ImGui::CollapsingHeader("RTXDI Settings") )
        {
#define RTXDI_RESTIR_RESET_ON_CHANGE(code) do { if (code) { m_ui.ResetAccumulation = true; m_ui.RTXDIRestirPreset = RTXDIRestirQualityPreset::Custom; } } while(false)
#define RTXDI_RESTIR_PT_RESET_ON_CHANGE(code) do { if (code) { m_ui.ResetAccumulation = true; m_ui.RTXDIRestirPTPreset = RTXDIRestirPTQualityPreset::Custom; } } while(false)

            ImGui::TextColored(categoryColor, "ReGIR");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );

                if (m_ui.ActualUseReSTIRDI())
                {
		            ImGui::PushItemWidth(layout.defItemWidth);
       
		            RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("Number of Build Samples", (int*)&m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples));
		            m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples = dm::clamp(m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples, 0u, 128u);
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Cell Size", &m_ui.RTXDI.regir.regirDynamicParameters.regirCellSize, 0.1f, 2.f));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Sampling Jitter", &m_ui.RTXDI.regir.regirDynamicParameters.regirSamplingJitter, 0.f, 1.f));

                    ImGui::PopItemWidth();
                }
                else
                    ImGui::Text("Not used/enabled");
            }

            ImGui::TextColored(categoryColor, "ReSTIR DI");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                if( m_ui.ActualUseReSTIRDI() )
                {
                    ImGui::PushItemWidth(layout.defItemWidth);

                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Resampling Mode", (int*)&m_ui.RTXDI.restirDI.resamplingMode,
                        "Disabled\0Temporal\0Spatial\0Temporal & Spatial\0Fused\0\0"));
       
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Spatial Bias Correction", (int*)&m_ui.RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection,
                        "Off\0Basic\0Pairwise\0Ray Traced\0\0"));
		
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Temporal Bias Correction", (int*)&m_ui.RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection,
                        "Off\0Basic\0Pairwise\0Ray Traced\0\0"));
		
		            RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Local Light Sampling Mode", (int*)&m_ui.RTXDI.restirDI.initialSamplingParams.localLightSamplingMode,
			            "Uniform\0Power RIS\0ReGIR RIS\0\0"));

                    if (m_ui.RTXDI.restirDI.initialSamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS)
                    {
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("ReGIR Mode", (int*)&m_ui.RTXDI.regir.regirStaticParams.Mode,
                            "Disabled\0Grid\0Onion\0\0"));
                    }
        
                    ImGui::PopItemWidth();

                    ImGui::PushItemWidth(layout.defItemWidth*0.8f);
            
                    ImGui::Text("Number of Primary Samples: ");

                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("ReGir", (int*)&m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples));
                        m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples = dm::clamp(m_ui.RTXDI.regir.regirDynamicParameters.regirNumBuildSamples, 0u, 32u);
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("Local Light", (int*)&m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples));
		                m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = dm::clamp(m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples, 0u, 32u);
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("BRDF", (int*)&m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples));
		                m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = dm::clamp(m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples, 0u, 32u);
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("Infinite Light", (int*)&m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples));
		                m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = dm::clamp(m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples, 0u, 32u);
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::InputInt("Environment Light", (int*)&m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples));
		                m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = dm::clamp(m_ui.RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples, 0u, 32u);
                    }
                    RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Initial visibility test", &m_ui.RTXDI.restirDI.initialSamplingParams.enableInitialVisibility));
    
                    if (ImGui::CollapsingHeader("Fine Tuning"))
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                        ImGui::PushItemWidth(layout.defItemWidth);
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("BRDF Cut-off", &m_ui.RTXDI.restirDI.initialSamplingParams.brdfCutoff, 0.0f, 1.0f));
                        ImGui::Separator();
                        RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Use Permutation Sampling", &m_ui.RTXDI.restirDI.temporalResamplingParams.enablePermutationSampling));
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Temporal Depth Threshold", &m_ui.RTXDI.restirDI.temporalResamplingParams.temporalDepthThreshold, 0.f, 1.f));
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Temporal Normal Threshold", &m_ui.RTXDI.restirDI.temporalResamplingParams.temporalNormalThreshold, 0.f, 1.f));
			            RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Boiling Filter Strength", &m_ui.RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength, 0.f, 1.f));
                        RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Discard Invisible Temporal Samples", &m_ui.RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples));
                        ImGui::Separator();
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderInt("Spatial Samples", (int*)&m_ui.RTXDI.restirDI.spatialResamplingParams.numSpatialSamples, 0, 8));
			            RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderInt("Disocclusion Samples", (int*)&m_ui.RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples, 0, 8));
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Sampling Radius", &m_ui.RTXDI.restirDI.spatialResamplingParams.spatialSamplingRadius, 0.f, 64.f));
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Depth Threshold", &m_ui.RTXDI.restirDI.spatialResamplingParams.spatialDepthThreshold, 0.f, 1.f));
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Normal Threshold", &m_ui.RTXDI.restirDI.spatialResamplingParams.spatialNormalThreshold, 0.f, 1.f));
			            RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Discount Naive Samples", &m_ui.RTXDI.restirDI.spatialResamplingParams.discountNaiveSamples));
			            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prevents samples which are from the current frame or have no reasonable temporal history merged being spread to neighbors");
                        ImGui::Separator();
                        RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::DragFloat("Ray Epsilon", &m_ui.RTXDI.rayEpsilon, 0.0001f, 0.0001f, 0.01f, "%.4f"));
                        ImGui::PopItemWidth();
                    }

                    ImGui::PopItemWidth();
                }
                else
                    ImGui::Text("Not used/enabled");
            }

            ImGui::TextColored(categoryColor, "ReSTIR GI");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                if (m_ui.ActualUseReSTIRGI())
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                    ImGui::PushItemWidth(layout.defItemWidth);
		            RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Resampling Mode", (int*)&m_ui.RTXDI.restirGI.resamplingMode,
			            "Disabled\0Temporal\0Spatial\0Temporal & Spatial\0Fused\0\0"));
                    ImGui::TextWrapped("Please note: there's a bug in ReSTIRGIContext::UpdateBufferIndices or similar which breaks 'Disabled' or one or the other sampling modes.");
                    ImGui::Separator();
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderInt("History Length ##GI", (int*)&m_ui.RTXDI.restirGI.temporalResamplingParams.maxHistoryLength, 0, 64));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderInt("Max Reservoir Age ##GI", (int*)&m_ui.RTXDI.restirGI.temporalResamplingParams.maxReservoirAge, 0, 100));
                    RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Permutation Sampling ##GI", &m_ui.RTXDI.restirGI.temporalResamplingParams.enablePermutationSampling));
                    RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Fallback Sampling ##GI", &m_ui.RTXDI.restirGI.temporalResamplingParams.enableFallbackSampling));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Boiling Filter Strength##GI", &m_ui.RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength, 0.f, 1.f));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Temporal Bias Correction ##GI", (int*)&m_ui.RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode,
                        "Off\0Basic\0Ray Traced\0"));
                    ImGui::Separator();
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderInt("Spatial Samples ##GI", (int*)&m_ui.RTXDI.restirGI.spatialResamplingParams.numSpatialSamples, 0, 8));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Sampling Radius ##GI", &m_ui.RTXDI.restirGI.spatialResamplingParams.spatialSamplingRadius, 1.f, 64.f));
                    RTXDI_RESTIR_RESET_ON_CHANGE(ImGui::Combo("Spatial Bias Correction ##GI", (int*)&m_ui.RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode, "Off\0Basic\0Ray Traced\0"));
                    ImGui::Separator();
                    RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Final Visibility ##GI", &m_ui.RTXDI.restirGI.finalShadingParams.enableFinalVisibility));
                    RTXDI_RESTIR_RESET_ON_CHANGE(CheckboxUInt32("Final MIS ##GI", &m_ui.RTXDI.restirGI.finalShadingParams.enableFinalMIS));

                    ImGui::PopItemWidth();
                }
            }

            ImGui::TextColored(categoryColor, "ReSTIR PT");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                if (m_ui.ActualUseReSTIRPT())
                {
                    ImGui::PushItemWidth(layout.defItemWidth);

                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::Combo("Resampling Mode ##PT", (int*)&m_ui.RTXDI.restirPT.resamplingMode,
                        "Disabled\0Temporal\0Spatial\0Temporal & Spatial\0\0"));

                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Initial Samples ##PT", (int*)&m_ui.RTXDI.restirPT.initialSamplingParams.numInitialSamples, 1, 8));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Max Bounce Depth ##PT", (int*)&m_ui.RTXDI.restirPT.initialSamplingParams.maxBounceDepth, 1, 12));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Max RC Vertex Length ##PT", (int*)&m_ui.RTXDI.restirPT.initialSamplingParams.maxRcVertexLength, 1, 12));

                    int reconnectionMode = int(m_ui.RTXDI.restirPT.reconnectionParams.reconnectionMode);
                    bool reconnectionChanged = ImGui::Combo("Reconnection Mode ##PT", &reconnectionMode, "Fixed Threshold\0Footprint\0\0");
                    if (reconnectionChanged)
                        m_ui.RTXDI.restirPT.reconnectionParams.reconnectionMode = RTXDI_PTReconnectionMode(reconnectionMode);
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(reconnectionChanged);

                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Min Connection Footprint ##PT", &m_ui.RTXDI.restirPT.reconnectionParams.minConnectionFootprint, 0.0f, 0.1f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Min PDF Roughness ##PT", &m_ui.RTXDI.restirPT.reconnectionParams.minPdfRoughness, 0.0f, 0.5f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Roughness Threshold ##PT", &m_ui.RTXDI.restirPT.reconnectionParams.roughnessThreshold, 0.0f, 0.5f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Distance Threshold ##PT", &m_ui.RTXDI.restirPT.reconnectionParams.distanceThreshold, 0.0f, 10.0f));

                    ImGui::Separator();
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("History Length ##PT", (int*)&m_ui.RTXDI.restirPT.temporalResamplingParams.maxHistoryLength, 0, 64));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Max Reservoir Age ##PT", (int*)&m_ui.RTXDI.restirPT.temporalResamplingParams.maxReservoirAge, 0, 100));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(CheckboxUInt32("Fallback Sampling ##PT", &m_ui.RTXDI.restirPT.temporalResamplingParams.enableFallbackSampling));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(CheckboxUInt32("Permutation Sampling ##PT", &m_ui.RTXDI.restirPT.temporalResamplingParams.enablePermutationSampling));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Temporal Depth Threshold ##PT", &m_ui.RTXDI.restirPT.temporalResamplingParams.depthThreshold, 0.0f, 1.0f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Temporal Normal Threshold ##PT", &m_ui.RTXDI.restirPT.temporalResamplingParams.normalThreshold, 0.0f, 1.0f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(CheckboxUInt32("Boiling Filter ##PT", &m_ui.RTXDI.restirPT.boilingFilterParams.enableBoilingFilter));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Boiling Filter Strength ##PT", &m_ui.RTXDI.restirPT.boilingFilterParams.boilingFilterStrength, 0.0f, 1.0f));

                    ImGui::Separator();
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Spatial Samples ##PT", (int*)&m_ui.RTXDI.restirPT.spatialResamplingParams.numSpatialSamples, 0, 8));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderInt("Disocclusion Samples ##PT", (int*)&m_ui.RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples, 0, 16));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Sampling Radius ##PT", &m_ui.RTXDI.restirPT.spatialResamplingParams.samplingRadius, 1.0f, 64.0f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Depth Threshold ##PT", &m_ui.RTXDI.restirPT.spatialResamplingParams.depthThreshold, 0.0f, 1.0f));
                    RTXDI_RESTIR_PT_RESET_ON_CHANGE(ImGui::SliderFloat("Spatial Normal Threshold ##PT", &m_ui.RTXDI.restirPT.spatialResamplingParams.normalThreshold, 0.0f, 1.0f));

                    ImGui::PopItemWidth();
                }
                else
                    ImGui::Text("Not used/enabled");
            }
#undef RTXDI_RESTIR_PT_RESET_ON_CHANGE
#undef RTXDI_RESTIR_RESET_ON_CHANGE
        }


}

void EditorUI::BuildStablePlanesPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Stable Planes (denoising layers)"))
        {
            if (m_ui.RealtimeMode)
            {
                ImGui::InputInt("Active stable planes", &m_ui.StablePlanesActiveCount);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many stable planes to allow - 1 is just standard denoising");
                m_ui.StablePlanesActiveCount = dm::clamp(m_ui.StablePlanesActiveCount, 1, (int)cStablePlaneCount);
                ImGui::InputInt("Max stable plane vertex depth", &m_ui.StablePlanesMaxVertexDepth);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("How deep the stable part of path tracing can go");
                m_ui.StablePlanesMaxVertexDepth = dm::clamp(m_ui.StablePlanesMaxVertexDepth, 2, (int)cStablePlaneMaxVertexIndex);
                ImGui::SliderFloat("Path split stop threshold", &m_ui.StablePlanesSplitStopThreshold, 0.0f, 2.0f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops splitting if more than this threshold throughput will be on a non-taken branch.\nActual threshold is this value divided by vertexIndex.");
                ImGui::Checkbox("Primary Surface Replacement", &m_ui.AllowPrimarySurfaceReplacement);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("When stable planes enabled, whether we can use PSR for the first (base) plane");
                ImGui::Checkbox("Suppress primary plane noisy specular", &m_ui.StablePlanesSuppressPrimaryIndirectSpecular);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This will suppress noisy specular to primary stable plane by specified amount\nbut only if at least 1 stable plane is also used on the same pixel.\nThis for ex. reduces secondary internal smudgy reflections from internal many bounces in a window.");
                ImGui::SliderFloat("Suppress primary plane noisy specular amount", &m_ui.StablePlanesSuppressPrimaryIndirectSpecularK, 0.0f, 1.0f);
                ImGui::SliderFloat("Non-primary plane anti-aliasing fallthrough", &m_ui.StablePlanesAntiAliasingFallthrough, 0.0f, 1.0f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divert some radiance on highly curved and edge areas from non-0 plane back\nto plane 0. This reduces aliasing on complex boundary bounces.");
            }
            else
            {
                ImGui::Text("Not available in reference mode");
            }
        }


}

void EditorUI::BuildStandaloneDenoiserPanel(const PanelLayout& layout)
{
        if (m_ui.ActualUseStandaloneDenoiser() && ImGui::CollapsingHeader("Standalone Denoiser (NRD)"))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

            ImGui::InputFloat("Disocclusion Threshold", &m_ui.NRDDisocclusionThreshold);
            ImGui::Checkbox("Use Alternate Disocclusion Threshold Mix", &m_ui.NRDUseAlternateDisocclusionThresholdMix);
            ImGui::InputFloat("Disocclusion Threshold Alt", &m_ui.NRDDisocclusionThresholdAlternate);
            ImGui::InputFloat("Radiance clamping", &m_ui.DenoiserRadianceClampK);

            ImGui::Separator();

            m_ui.NRDModeChanged = ImGui::Combo("Denoiser Mode", (int*)&m_ui.NRDMethod, "REBLUR\0RELAX\0\0");
            m_ui.NRDMethod = dm::clamp(m_ui.NRDMethod, (NrdConfig::DenoiserMethod)0, (NrdConfig::DenoiserMethod)1);

            if (ImGui::CollapsingHeader("Advanced Settings"))
            {
                if (m_ui.NRDMethod == NrdConfig::DenoiserMethod::REBLUR)
                {
                    // TODO: make sure these are updated to constants
                    ImGui::SliderFloat("Hit Distance A", &m_ui.ReblurSettings.hitDistanceParameters.A, 0.0f, 10.0f);
                    ImGui::SliderFloat("Hit Distance B", &m_ui.ReblurSettings.hitDistanceParameters.B, 0.0f, 10.0f);
                    ImGui::SliderFloat("Hit Distance C", &m_ui.ReblurSettings.hitDistanceParameters.C, 0.0f, 50.0f);
                    ImGui::SliderFloat("Hit Distance D", &m_ui.ReblurSettings.hitDistanceParameters.D, -50.0f, 0.0f);

                    ImGui::SliderFloat("Antilag Luminance Sigma Scale", &m_ui.ReblurSettings.antilagSettings.luminanceSigmaScale, 1.0f, 3.0f);
                    // ImGui::SliderFloat("Antilag Hit Distance Sigma Scale", &m_ui.ReblurSettings.antilagSettings.hitDistanceSigmaScale, 1.0f, 3.0f);
                    ImGui::SliderFloat("Antilag Luminance Sensitivity", &m_ui.ReblurSettings.antilagSettings.luminanceSensitivity, 0.001f, 1.0f);
                    // ImGui::SliderFloat("Antilag Hit Distance Sensitivity", &m_ui.ReblurSettings.antilagSettings.hitDistanceSensitivity, 0.001f, 1.0f);

                    ImGui::SliderInt("Max Accumulated Frames", (int*)&m_ui.ReblurSettings.maxAccumulatedFrameNum, 0, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
                    ImGui::SliderInt("Fast Max Accumulated Frames", (int*)&m_ui.ReblurSettings.maxFastAccumulatedFrameNum, 0, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
                    ImGui::SliderInt("History Fix Frames", (int*)&m_ui.ReblurSettings.historyFixFrameNum, 0, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);

                    ImGui::SliderFloat("Diffuse Prepass Blur Radius (pixels)", &m_ui.ReblurSettings.diffusePrepassBlurRadius, 0.0f, 100.0f);
                    ImGui::SliderFloat("Specular Prepass Blur Radius (pixels)", &m_ui.ReblurSettings.specularPrepassBlurRadius, 0.0f, 100.0f);
                    ImGui::SliderFloat("Min Blur Radius (pixels)", &m_ui.ReblurSettings.minBlurRadius, 0.0f, 100.0f);
                    ImGui::SliderFloat("Max Blur Radius (pixels)", &m_ui.ReblurSettings.maxBlurRadius, 0.0f, 100.0f);

                    ImGui::SliderFloat("Lobe Angle Fraction", &m_ui.ReblurSettings.lobeAngleFraction, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness Fraction", &m_ui.ReblurSettings.roughnessFraction, 0.0f, 1.0f);

                    ImGui::SliderFloat("Accumulation Roughness Threshold", &m_ui.ReblurSettings.responsiveAccumulationRoughnessThreshold, 0.0f, 1.0f);

                    //ImGui::SliderFloat("Stabilization Strength", &m_ui.ReblurSettings.stabilizationStrength, 0.0f, 1.0f);

                    ImGui::SliderFloat("Plane Distance Sensitivity", &m_ui.ReblurSettings.planeDistanceSensitivity, 0.0f, 1.0f);

                    // ImGui::Combo("Checkerboard Mode", (int*)&m_ui.ReblurSettings.checkerboardMode, "Off\0Black\0White\0\0");

                    // these are uint8_t and ImGUI takes a ptr to int32_t :(
                    int hitDistanceReconstructionMode = (int)m_ui.ReblurSettings.hitDistanceReconstructionMode;
                    ImGui::Combo("Hit Distance Reconstruction Mode", &hitDistanceReconstructionMode, "Off\0AREA_3X3\0AREA_5X5\0\0");
                    m_ui.ReblurSettings.hitDistanceReconstructionMode = (nrd::HitDistanceReconstructionMode)hitDistanceReconstructionMode;

                    ImGui::Checkbox("Enable Firefly Filter", &m_ui.ReblurSettings.enableAntiFirefly);

                    // ImGui::Checkbox("Enable Diffuse Material Test", &m_ui.ReblurSettings.enableMaterialTestForDiffuse);
                    // ImGui::Checkbox("Enable Specular Material Test", &m_ui.ReblurSettings.enableMaterialTestForSpecular);
                }
                else // m_ui.NRDMethod == NrdConfig::DenoiserMethod::RELAX
                {
                    ImGui::SliderFloat("Diffuse Prepass Blur Radius", &m_ui.RelaxSettings.diffusePrepassBlurRadius, 0.0f, 100.0f);
                    ImGui::SliderFloat("Specular Prepass Blur Radius", &m_ui.RelaxSettings.specularPrepassBlurRadius, 0.0f, 100.0f);

                    ImGui::SliderInt("Diffuse Max Accumulated Frames", (int*)&m_ui.RelaxSettings.diffuseMaxAccumulatedFrameNum, 0, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
                    ImGui::SliderInt("Specular Max Accumulated Frames", (int*)&m_ui.RelaxSettings.specularMaxAccumulatedFrameNum, 0, nrd::RELAX_MAX_HISTORY_FRAME_NUM);

                    ImGui::SliderInt("Diffuse Fast Max Accumulated Frames", (int*)&m_ui.RelaxSettings.diffuseMaxFastAccumulatedFrameNum, 0, 10);   // nrd::RELAX_MAX_HISTORY_FRAME_NUM
                    ImGui::SliderInt("Specular Fast Max Accumulated Frames", (int*)&m_ui.RelaxSettings.specularMaxFastAccumulatedFrameNum, 0, 10); // nrd::RELAX_MAX_HISTORY_FRAME_NUM

                    ImGui::SliderInt("History Fix Frame Num", (int*)&m_ui.RelaxSettings.historyFixFrameNum, 0, nrd::RELAX_MAX_HISTORY_FRAME_NUM);

                    ImGui::SliderFloat("Diffuse Edge Stopping Sensitivity", &m_ui.RelaxSettings.diffusePhiLuminance, 0.0f, 10.0f);
                    ImGui::SliderFloat("Specular Edge Stopping Sensitivity", &m_ui.RelaxSettings.specularPhiLuminance, 0.0f, 10.0f);

                    ImGui::SliderFloat("Lobe Angle Fraction", &m_ui.RelaxSettings.lobeAngleFraction, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness Fraction", &m_ui.RelaxSettings.roughnessFraction, 0.0f, 1.0f);

                    ImGui::SliderFloat("Specular Variance Boost", &m_ui.RelaxSettings.specularVarianceBoost, 0.0f, 1.0f);

                    ImGui::SliderFloat("Specular Lobe Angle Slack", &m_ui.RelaxSettings.specularLobeAngleSlack, 0.0f, 1.0f);

                    ImGui::SliderFloat("Normal Edge Stopping Power", &m_ui.RelaxSettings.historyFixEdgeStoppingNormalPower, 0.0f, 30.0f);

                    ImGui::SliderFloat("Clamping Color Box Sigma Scale", &m_ui.RelaxSettings.historyClampingColorBoxSigmaScale, 0.0f, 3.0f);

                    ImGui::SliderInt("Spatial Variance Estimation History Threshold", (int*)&m_ui.RelaxSettings.spatialVarianceEstimationHistoryThreshold, 0, nrd::RELAX_MAX_HISTORY_FRAME_NUM);

                    ImGui::SliderInt("Number of Atrous iterations", (int*)&m_ui.RelaxSettings.atrousIterationNum, 2, 8);

                    ImGui::SliderFloat("Diffuse Min Luminance Weight", &m_ui.RelaxSettings.diffuseMinLuminanceWeight, 0.0f, 1.0f);
                    ImGui::SliderFloat("Specular Min Luminance Weight", &m_ui.RelaxSettings.specularMinLuminanceWeight, 0.0f, 1.0f);

                    ImGui::SliderFloat("Edge Stopping Threshold", &m_ui.RelaxSettings.depthThreshold, 0.0f, 0.1f);

                    ImGui::SliderFloat("Confidence: Relaxation Multiplier", &m_ui.RelaxSettings.confidenceDrivenRelaxationMultiplier, 0.0f, 1.0f);
                    ImGui::SliderFloat("Confidence: Luminance Edge Stopping Relaxation", &m_ui.RelaxSettings.confidenceDrivenLuminanceEdgeStoppingRelaxation, 0.0f, 1.0f);
                    ImGui::SliderFloat("Confidence: Normal Edge Stopping Relaxation", &m_ui.RelaxSettings.confidenceDrivenNormalEdgeStoppingRelaxation, 0.0f, 1.0f);

                    ImGui::SliderFloat("Luminance Edge Stopping Relaxation", &m_ui.RelaxSettings.luminanceEdgeStoppingRelaxation, 0.0f, 1.0f);
                    ImGui::SliderFloat("Normal Edge Stopping Relaxation", &m_ui.RelaxSettings.normalEdgeStoppingRelaxation, 0.0f, 1.0f);

                    ImGui::SliderFloat("Roughness Edge Stopping Relaxation", &m_ui.RelaxSettings.roughnessEdgeStoppingRelaxation, 0.0f, 5.0f);

                    ImGui::SliderFloat("Antilag Acceleration Amount", &m_ui.RelaxSettings.antilagSettings.accelerationAmount, 0.0f, 1.0f);
                    ImGui::SliderFloat("Antilag Spatial Sigma Scale", &m_ui.RelaxSettings.antilagSettings.spatialSigmaScale, 0.0f, 5.0f);
                    ImGui::SliderFloat("Antilag Temporal Sigma Scale", &m_ui.RelaxSettings.antilagSettings.temporalSigmaScale, 0.0f, 5.0f);
                    ImGui::SliderFloat("Antilag Reset Amount", &m_ui.RelaxSettings.antilagSettings.resetAmount, 0.0f, 1.0f);

                    // ImGui::Combo("Checkerboard Mode", (int*)&m_ui.RelaxSettings.checkerboardMode, "Off\0Black\0White\0\0");

                    int hitDistanceReconstructionMode = (int)m_ui.RelaxSettings.hitDistanceReconstructionMode;  // these are uint8_t and ImGUI takes a ptr to int32_t :(
                    ImGui::Combo("Hit Distance Reconstruction Mode", &hitDistanceReconstructionMode, "Off\0AREA_3X3\0AREA_5X5\0\0");
                    m_ui.RelaxSettings.hitDistanceReconstructionMode = (nrd::HitDistanceReconstructionMode)hitDistanceReconstructionMode;

                    ImGui::Checkbox("Enable Firefly Filter", &m_ui.RelaxSettings.enableAntiFirefly);

                    ImGui::Checkbox("Roughness Edge Stopping", &m_ui.RelaxSettings.enableRoughnessEdgeStopping);

                    // ImGui::Checkbox("Enable Diffuse Material Test", &m_ui.RelaxSettings.enableMaterialTestForDiffuse);
                    // ImGui::Checkbox("Enable Specular Material Test", &m_ui.RelaxSettings.enableMaterialTestForSpecular);
                }

                // Not really needed for now since we have reference codepath, but it could be used to debug some of the NRD codepaths so leaving in as a reminder
                // ImGui::Checkbox("Reference Accumulation", &m_ui.NRDReferenceSettings.maxAccumulatedFrameNum);
            }
        }


}


} // namespace caustica::editor

