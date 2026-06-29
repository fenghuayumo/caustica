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

void EditorUI::BuildLightingPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Light pre-processing and sampling", 0/*ImGuiTreeNodeFlags_DefaultOpen*/))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

            if (!m_ui.UseNEE )
                ImGui::TextColored(warnColor, "NOTE: NEE inactive (enable in `Path Tracer -> Next Event Estimation` settings).");

            ImGui::TextColored(categoryColor, "Info and statistics:");

            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                if (auto& lightsBaker = m_sceneEditor.GetLightingPasses().lightsBaker(); lightsBaker != nullptr)
                    m_ui.ResetAccumulation |= lightsBaker->InfoGUI(layout.indent);
            }


            //ImGui::TextColored(categoryColor, "Distant lighting (envmap+directional):");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););
                if (ImGui::CollapsingHeader("Distant lighting (envmap+directional)", 0/*ImGuiTreeNodeFlags_DefaultOpen*/))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););
                    if (auto& envMapBaker = m_sceneEditor.GetLightingPasses().envMapBaker(); envMapBaker != nullptr)
                        m_ui.ResetAccumulation |= envMapBaker->DebugGUI(layout.indent);
                }
            }

            ImGui::TextColored(categoryColor, "Importance sampling:");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                if (auto& lightsBaker = m_sceneEditor.GetLightingPasses().lightsBaker(); lightsBaker != nullptr)
                {
                    if( m_ui.NEEType != 2 )
                    {
                        ImGui::TextWrapped("NOTE: NEE-AT inactive (enable in `Path Tracer -> Next Event Estimation` settings).");
                    }
                    else
                    {
                        ImGui::TextColored(categoryColor, "NEE-AT settings:");
                        {
                            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););

                            RESET_ON_CHANGE(ImGui::SliderFloat("Global feedback weight", &m_ui.NEEAT_GlobalTemporalFeedbackWeight, 0.0f, 0.95f));
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much to rely on last frame's usage statistic as opposed to simple power based sampling.\nSome power based sampling is essential to allow new lights to be considered.");

                            RESET_ON_CHANGE(ImGui::SliderFloat("Local to global sampler ratio", &m_ui.NEEAT_LocalToGlobalSampleRatio, 0.0f, 0.95f));
                    
                            uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);
                            uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);
                    
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("When drawing new light candidate samples, how many to draw from Global versus Local (tile) samplers.\n"
                                                                            "Current total candidate sample count is %d, and out of those %d will be Local and %d will be Global", 
                                                                                m_ui.NEECandidateSamples, localCandidateSamples, globalCandidateSamples);

                            // ImGui::SliderFloat("BSDF vs NEE-AT MIS boost", &m_ui.NEEAT_MIS_Boost, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                            // if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tweak the MIS to give more power to NEE-AT (>1) or to BSDF sampled emissives (<1);\nuseful since NEE-AT is shadow aware and boosting it can provide better overall sampling quality");
                            
                            ImGui::SliderFloat("Distant vs Local initial importance", &m_ui.NEEAT_Distant_vs_Local_Importance, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The higher the setting, the more initial importance will be given to environment map / sunlight vs local scene lights and vice versa.");
                        }
                    }
                
                    ImGui::TextColored(categoryColor, "Debugging:");
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                        if (auto& lightsBaker = m_sceneEditor.GetLightingPasses().lightsBaker(); lightsBaker != nullptr)
                            m_ui.ResetAccumulation |= lightsBaker->DebugGUI(layout.indent);
                    }
                }
            }
        }


}

void EditorUI::BuildPathTracerPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Path Tracer", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

            int modeIndex = (m_ui.RealtimeMode)?(1):(0);
            if (ImGui::Combo("Mode", &modeIndex, "Reference\0Realtime\0\0"))
            {
                m_ui.RealtimeMode = (modeIndex!=0);
                m_ui.ResetAccumulation = true;
            }

            ImGui::TextColored(categoryColor, "Setup:");
            {   
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
            
                if (m_ui.RealtimeMode)
                {
                    if (ImGui::Button("Reset##RTMACC"))
                        m_ui.ResetRealtimeCaches = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset all temporal caches in denoising, lighting and etc");
                    ImGui::SameLine();
            
                    {
                        UI_SCOPED_DISABLE( (m_ui.ActualUseReSTIRDI() || m_ui.ActualUseReSTIRGI() || m_ui.ActualUseReSTIRPT()) );
                        ImGui::InputInt("Samples per pixel", &m_ui.RealtimeSamplesPerPixel); 
                        m_ui.RealtimeSamplesPerPixel = dm::clamp(m_ui.RealtimeSamplesPerPixel, 1, 64);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                            ImGui::SetTooltip("How many full paths to trace per pixel from the primary surface\n(camera ray is not re-cast so there is no added AA)\n(currently incompatible with ReSTIR DI, ReSTIR GI & ReSTIR PT)");
                    }
                }
                else
                {
                    RESET_ON_CHANGE( ImGui::Button("Reset##REFMACC") );
                    ImGui::SameLine();
                    RESET_ON_CHANGE( ImGui::InputInt("Sample count", &m_ui.AccumulationTarget) );
                    m_ui.AccumulationTarget = dm::clamp(m_ui.AccumulationTarget, 1, 4 * 1024 * 1024); // this max is beyond float32 precision threshold; expect some banding creeping in when using more than 500k samples
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of path samples per pixel to collect");
                    ImGui::Text("Accumulated samples: %d (out of %d target)", m_sceneEditor.GetAccumulationSampleIndex(), m_ui.AccumulationTarget);
                    ImGui::Text("(avg frame time: %.3fms)", m_sceneEditor.GetAvgTimePerFrame() * 1000.0f);

                    RESET_ON_CHANGE(ImGui::Checkbox("Pre-warm real-time caches", &m_ui.AccumulationPreWarmRealtimeCaches));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("If enabled, various lighting and etc systems will be pre-warmed before sample 0 is \naccumulated; otherwise they're reset and initial few samples will be lower quality.");

                    RESET_ON_CHANGE(ImGui::Checkbox("Jitter anti-aliasing", &m_ui.AccumulationAA));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each sample will have a random, per pixel jitter emulating box filter\nTODO: add option for Gaussian distribution for better AA");

                    m_ui.ResetRealtimeCaches |= m_ui.ResetAccumulation; // if there's a reset for any reason whilst we're in reference mode, reset the realtime caches too for determinism
                }

                RESET_ON_CHANGE(ImGui::InputInt("Max bounces", &m_ui.BounceCount));
                m_ui.BounceCount = dm::clamp(m_ui.BounceCount, 0, MAX_BOUNCE_COUNT);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max number of all bounces (including NEE and diffuse bounces)");
                RESET_ON_CHANGE(ImGui::InputInt("Max diffuse bounces", &m_ui.DiffuseBounceCount));
                m_ui.DiffuseBounceCount = dm::clamp(m_ui.DiffuseBounceCount, 0, MAX_BOUNCE_COUNT);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max number of diffuse bounces (diffuse lobe and specular with roughness > 0.25 or similar depending on settings)");

                if (m_ui.RealtimeMode)
                {
                    RESET_ON_CHANGE( ImGui::Checkbox("FireflyFilter (realtime)", &m_ui.RealtimeFireflyFilterEnabled) );
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable smart firefly filter that clamps max radiance based on probability heuristic.");
                    if (m_ui.RealtimeFireflyFilterEnabled)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        RESET_ON_CHANGE( ImGui::InputFloat("FF Threshold", &m_ui.RealtimeFireflyFilterThreshold, 0.01f, 0.1f, "%.5f") );
                        m_ui.RealtimeFireflyFilterThreshold = dm::clamp(m_ui.RealtimeFireflyFilterThreshold, 0.00001f, 1000.0f);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Better light importance sampling allows for setting higher firefly filter threshold and conversely.");
                        //ImGui::SameLine();
                        //RESET_ON_CHANGE( ImGui::Checkbox("RX", &m_ui.RealtimeFireflyFilterRelaxOnNonNoisy) ); 
                        //if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relax on non-noisy (direct, stable) radiance: clamp value will be FIREFLY_FILTER_RELAX_ON_NON_NOISY_K times bigger - helps with blooms");
                    }
                }
                else
                {
                    RESET_ON_CHANGE( ImGui::Checkbox("FireflyFilter (reference *)", &m_ui.ReferenceFireflyFilterEnabled) );
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable smart firefly filter that clamps max radiance based on probability heuristic.\n* when both tonemapping autoexposure and firefly filter are enabled\nin reference mode, results are no longer deterministic!");
                    if (m_ui.ReferenceFireflyFilterEnabled)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        RESET_ON_CHANGE( ImGui::InputFloat("FF Threshold", &m_ui.ReferenceFireflyFilterThreshold, 0.1f, 0.2f, "%.5f") );
                        m_ui.ReferenceFireflyFilterThreshold = dm::clamp(m_ui.ReferenceFireflyFilterThreshold, 0.01f, 1000.0f);
                    }
                }

                RESET_ON_CHANGE( ImGui::InputFloat("Texture MIP bias", &m_ui.TexLODBias) );

                RESET_ON_CHANGE(ImGui::InputInt("Diffuse sample envmap MIP level", &m_ui.EnvironmentMapDiffuseSampleMIPLevel));    m_ui.EnvironmentMapDiffuseSampleMIPLevel = dm::clamp(m_ui.EnvironmentMapDiffuseSampleMIPLevel, 0, 16);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the specific MIP level to sample environment map texture during light sampling and for main path terminating\ninto sky after a diffuse scatter. Only 0 produces unbiased results.");

                RESET_ON_CHANGE(ImGui::Checkbox("Use Russian Roulette early out", &m_ui.EnableRussianRoulette));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables stochastic path termination for low throughput diffuse paths");
            }

            ImGui::TextColored(categoryColor, "Post processing:");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );

                if (m_ui.RealtimeMode)
                {
#if CAUSTICA_WITH_ANY_DLSS
                    const bool dlssAvailable = m_ui.IsDLSSSuported;
                    const bool dlssRRAvailable = m_ui.IsDLSSRRSupported; 
#else
                    const bool dlssAvailable = false;
                    const bool dlssRRAvailable = false;
#endif
                    const char* items[] = { "Disabled", "TAA", "DLSS", "DLSS-RR" };

                    const int itemCount = IM_ARRAYSIZE(items);

                    m_ui.RealtimeAA = dm::clamp(m_ui.RealtimeAA, 0, dlssAvailable ? itemCount : 1);

                    if (ImGui::BeginCombo("AA/SR/Denoising", items[m_ui.RealtimeAA]))
                    {
                        for (int i = 0; i < itemCount; i++)
                        {
                            bool enabled = false;
                            enabled |= i <2;
                            enabled |= (i == 2) && dlssAvailable;
                            enabled |= (i == 3) && dlssRRAvailable;
                            UI_SCOPED_DISABLE(!enabled);

                            bool isSelected = (m_ui.RealtimeAA == i);
                            if (ImGui::Selectable(items[i], isSelected))
                                m_ui.RealtimeAA = i;
                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(
                        "TAA        - generic temporal anti-aliasing\n"
                        "DLSS       - Nvidia Deep Learning Super Sampling (lower resolution render + upscale)\n"
                        "DLAA       - Nvidia Deep Learning Anti Aliasing (full resolution render)\n"
                        "DLSS-RR    - DLSS + Ray Reconstruction (lower resolution render + denoise & upscale)\n"
                        "\nIndividual DLSS options available under global `DLSS` options"
                    );

#if CAUSTICA_WITH_ANY_DLSS
                    if (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent); ImGui::PushID("PPDLSSQual");,  ImGui::Unindent(layout.indent); ImGui::PopID(););
                        m_ui.DLSSMode = DLSSModeUI(m_ui.DLSSMode);
                    }
#endif

                    {
                        UI_SCOPED_DISABLE(!m_ui.RealtimeMode || m_ui.RealtimeAA==3);
                        bool notTrue = false;
                        ImGui::Checkbox("Use standalone denoiser (NRD)", (m_ui.RealtimeAA==3)?(&notTrue):(&m_ui.StandaloneDenoiser));
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Enables NVIDIA Real-Time Denoisers (NRD) that execute before TAA/DLSS/DLAA pass\nNote: no built-in denoiser available in 'Reference' mode, however \n'Photo mode screenshot' button launches external denoiser!");
                    }
                }
                else // !m_ui.RealtimeMode
                {
#if CAUSTICA_WITH_OIDN
                    bool oidnChanged = false;
                    oidnChanged |= ImGui::Checkbox("Use OIDN denoiser", &m_ui.ReferenceOIDNDenoiser);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs Intel Open Image Denoise once after the Reference accumulation target is reached.\nThe denoised HDR result is reused until accumulation is reset.");

                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                        UI_SCOPED_DISABLE(!m_ui.ReferenceOIDNDenoiser);

                        oidnChanged |= ImGui::Checkbox("Use GPU", &m_ui.ReferenceOIDNUseGPU);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Uses OIDN GPU denoising when a supported CUDA/HIP/SYCL device is available; otherwise falls back to CPU.");

                        UI_SCOPED_DISABLE(true);
                        ImGui::Combo("Denoiser", &m_ui.ReferenceOIDNDenoiserType, "OpenImageDenoise\0\0");
                    }

                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                        UI_SCOPED_DISABLE(!m_ui.ReferenceOIDNDenoiser);

                        oidnChanged |= ImGui::Combo("Passes", &m_ui.ReferenceOIDNPasses, "Color Only\0Albedo\0Albedo + Normal\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Selects which auxiliary OIDN guide passes are used when available.");

                        oidnChanged |= ImGui::Combo("Prefilter", &m_ui.ReferenceOIDNPrefilter, "None\0Fast\0Accurate\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Prefilters noisy auxiliary guide passes before beauty denoising.");

                        oidnChanged |= ImGui::Combo("Quality", &m_ui.ReferenceOIDNQuality, "Fast\0Balanced\0High\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("OIDN quality/performance mode.");
                    }

                    if (oidnChanged)
                        m_ui.ReferenceOIDNDenoiserChanged = true;
#else
                    {
                        bool oidnDisabled = false;
                        UI_SCOPED_DISABLE(true);
                        ImGui::Checkbox("Use OIDN denoiser", &oidnDisabled);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("OIDN support is disabled in this build. Enable CAUSTICA_WITH_OIDN in CMake.");
#endif

                    if (ImGui::Button("Photo mode screenshot"))
                        m_ui.ExperimentalPhotoModeScreenshot = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Experimental: Saves a photo.bmp next to where .exe is and applies\n"
                        "denoising using legacy command line denoiser wrappers if installed.\n"
                        "For integrated HDR OIDN denoising, enable the OIDN checkbox above.\n"
                        "No guidance buffers are used and color is in LDR (so not as high quality\n"
                        "as it could be - will get improved in the future). \n"
                        "Command line denoiser wrapper tools by Declan Russel, available at:\n"
                        "https://github.com/DeclanRussell/NvidiaAIDenoiser\n"
                        "https://github.com/DeclanRussell/IntelOIDenoiser");
                }
                {
                    ImGui::Checkbox("Enable tone mapping", &m_ui.EnableToneMapping);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Full tone mapping settings available under global `Tone Mapping` options");
                }
            }

            ImGui::TextColored(categoryColor, "Light sampling:");
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                if (m_ui.RealtimeMode)
                {
                    if (m_ui.UseReSTIRGI && m_ui.UseReSTIRPT)
                        m_ui.UseReSTIRGI = false;

                    {
                        bool nullCheckbox = false;
                        bool disabled = !m_ui.UseNEE || (m_ui.RealtimeAA==3 && m_ui.DisableReSTIRsWithDLSSRR);
                        UI_SCOPED_DISABLE(disabled);
                        RESET_ON_CHANGE(ImGui::Checkbox("Use ReSTIR DI (RTXDI)", (disabled)?&nullCheckbox:&m_ui.UseReSTIRDI));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_ui.DisableReSTIRsWithDLSSRR = !m_ui.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR DI (RTXDI) requires Next Event Estimation to be enabled\nand this implementation is currently not tuned to work with DLSS-RR");

                    {
                        bool nullCheckbox = false;
                        bool disabled = m_ui.RealtimeAA==3 && m_ui.DisableReSTIRsWithDLSSRR;
                        UI_SCOPED_DISABLE( disabled );
                        const bool changed = ImGui::Checkbox("Use ReSTIR GI (RTXDI)", (disabled)?&nullCheckbox:&m_ui.UseReSTIRGI);
                        RESET_ON_CHANGE(changed);
                        if (changed && !disabled && m_ui.UseReSTIRGI)
                            m_ui.UseReSTIRPT = false;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_ui.DisableReSTIRsWithDLSSRR = !m_ui.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR GI and ReSTIR PT are mutually exclusive.\nReSTIR GI is currently not tuned to work well with DLSS-RR\nUse middle mouse button to enable anyway");

                    {
                        bool nullCheckbox = false;
                        bool disabled = m_ui.RealtimeAA==3 && m_ui.DisableReSTIRsWithDLSSRR;
                        UI_SCOPED_DISABLE(disabled);
                        const bool changed = ImGui::Checkbox("Use ReSTIR PT (RTXDI)", (disabled)?&nullCheckbox:&m_ui.UseReSTIRPT);
                        RESET_ON_CHANGE(changed);
                        if (changed && !disabled && m_ui.UseReSTIRPT)
                            m_ui.UseReSTIRGI = false;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_ui.DisableReSTIRsWithDLSSRR = !m_ui.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR PT and ReSTIR GI are mutually exclusive.\nReSTIR PT is currently not tuned to work well with DLSS-RR\nUse middle mouse button to enable anyway");

                    ImGui::PushItemWidth(layout.defItemWidth);
                    if (ImGui::Combo("ReSTIR DI/GI Preset", (int*)&m_ui.RTXDIRestirPreset,
                        "(Custom)\0Fast\0Medium\0Unbiased\0Ultra\0Reference\0\0"))
                    {
                        m_ui.ApplyRTXDIRestirPreset();
                    }
                    if (ImGui::Combo("ReSTIR PT Preset", (int*)&m_ui.RTXDIRestirPTPreset,
                        "(Custom)\0Fast\0Medium\0Ultra\0\0"))
                    {
                        m_ui.ApplyRTXDIRestirPTPreset();
                    }
                    ImGui::PopItemWidth();
                }

                RESET_ON_CHANGE(ImGui::Checkbox("Use Next Event Estimation", &m_ui.UseNEE));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables NEE a.k.a. direct light importance sampling (this includes ReSTIR DI but not ReSTIR GI)\nNote: analytic lights currently only come out of NEE so they will be missing when NEE is disabled");

                if (m_ui.UseNEE)
                {
                    ImGui::TextColored(categoryColor, "NEE settings: ");
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        RESET_ON_CHANGE(ImGui::Combo("Sampling technique", (int*)&m_ui.NEEType, "Uniform\0Power+\0NEE-AT\0\0"));
                        m_ui.NEEType = dm::clamp(m_ui.NEEType, 0, 2);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light importance sampling technique to use for NEE.\nNote: Additional NEE-AT settings are exposed in 'Lighting -> NEE-AT' UI section.");
    
                        RESET_ON_CHANGE(ImGui::InputInt("Candidate samples", &m_ui.NEECandidateSamples, 1));
                        if (ImGui::IsItemHovered()) 
                        {
                            if (m_ui.NEEType != 2)
                                ImGui::SetTooltip("This is the number of light samples weighted with BSDF used to pick each full sample.");
                            else
                            {
                                uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);
                                uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);

                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is the number of light samples weighted with BSDF used to pick each full sample.\n"
                                                                              "Out of those %d will be Local and %d will be Global NEE-AT samples", localCandidateSamples, globalCandidateSamples);
                            }
                        }
                        m_ui.NEECandidateSamples = dm::clamp(m_ui.NEECandidateSamples, 1, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);

                        RESET_ON_CHANGE(ImGui::InputInt("Full samples", &m_ui.NEEFullSamples, 1));
                        m_ui.NEEFullSamples = dm::clamp(m_ui.NEEFullSamples, 0, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is the number of light samples to shadow test and integrate\nNote: Maximum total number of samples is 63");

                        RESET_ON_CHANGE(ImGui::Combo("MIS Type", (int*)&m_ui.NEEMISType, "Full\0ApproxInRealtime\0Approximate\0\0"));
                        m_ui.NEEMISType = dm::clamp(m_ui.NEEMISType, 0, 2);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path (BSDF) vs light sampler Multiple Importance Sampling approach.\n'Approximate' is faster and easier to implement but more noisy, with \nthe impact of noise especially detrimental in reference accumulation.");
                    }
                }
            }

            if (ImGui::CollapsingHeader("PT: Advanced Settings", 0))
            {
                ImGui::TextColored(categoryColor, "Features:");
                ImGui::Combo("Nested Dielectrics", (int*)&m_ui.NestedDielectricsQuality, "Off\0Fast\0Quality\0"); m_ui.NestedDielectricsQuality = clamp( m_ui.NestedDielectricsQuality, 0, 2 );
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Priority-based nested dielectrics; 'Quality' allows for more \ncorrect rejections, 'Fast' is.. well, faster.");
                if (m_ui.RealtimeAA == 3)
                {
                    RESET_ON_CHANGE(ImGui::InputFloat("RR brightness clamp", &m_ui.DLSSRRBrightnessClampK));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("RR doesn't handle too bright (relatively) areas, causing unwanted noise;\nEnabling this will clamp brightness at the expense of bloom.\nTODO: replace this with local tonemap or similar");
                }

                ImGui::TextColored(categoryColor, "Performance:");
                {
                    if (m_NVAPI_SERSupported)
                    {
                        RESET_ON_CHANGE(ImGui::Checkbox("NVAPI HitObject codepath", &m_ui.NVAPIHitObjectExtension)); // <- while there's no need to reset accumulation since this is a performance only feature, leaving the reset in for testing correctness
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("If disabled, traditional TraceRay path is used.\nIf enabled, TraceRayInline->MakeHit->ReorderThread->InvokeHit approach is used!");
                        if (m_ui.NVAPIHitObjectExtension)
                        {
                            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                            ImGui::Checkbox("NVAPI ReorderThreads", &m_ui.NVAPIReorderThreads);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables/disables the actual ReorderThread call in the shader.");
                        }
                        if (m_ui.NVAPIHitObjectExtension)
                            m_ui.DXHitObjectExtension = false;
                    }
                    else
                    {
                        ImGui::Text("<NVAPI Hit Object Extension not supported>");
                        m_ui.NVAPIHitObjectExtension = false;
                    }

#if CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619
                    if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
                    {
                        RESET_ON_CHANGE(ImGui::Checkbox("dx::HitObject codepath", &m_ui.DXHitObjectExtension));
                        if (m_ui.DXHitObjectExtension)
                        {
                            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                            RESET_ON_CHANGE(ImGui::Checkbox("dx::MaybeReorderThreads ", &m_ui.DXMaybeReorderThreads));
                        }
                        if (m_ui.DXHitObjectExtension)
                            m_ui.NVAPIHitObjectExtension = false;
                    }
#endif
          
                    RESET_ON_CHANGE(ImGui::Checkbox("Use explicit fp16 types", &m_ui.UseFp16Types));

                    RESET_ON_CHANGE(ImGui::Checkbox("Enable LD sampler for BSDF", &m_ui.EnableLDSamplerForBSDF));
                }
            }
        }
}


} // namespace caustica::editor

