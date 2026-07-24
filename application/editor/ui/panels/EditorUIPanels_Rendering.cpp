#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
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
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

void EditorUI::BuildLightingPanel(const PanelLayout& layout)
{
        RAII_SCOPE(ImGui::PushID("LightingPanel");, ImGui::PopID(););
        if (ImGui::CollapsingHeader("Light pre-processing and sampling", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

            if (auto& lightSamplingCache = caustica::editor::requireWorldRenderer(m_sceneEditor).lightingPasses().lightSampling(); lightSamplingCache != nullptr)
                m_settings.ResetAccumulation |= lightSamplingCache->infoGUI(layout.indent);

            if (!m_settings.UseNEE)
                ImGui::TextColored(warnColor, "NEE inactive — enable in Path Tracer.");
            else if (m_settings.NEEType != 2)
                ImGui::TextColored(warnColor, "NEE-AT inactive — enable in Path Tracer.");

            if (ImGui::CollapsingHeader("Distant lighting (envmap+directional)"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                if (auto& envMapProcessor = caustica::editor::requireWorldRenderer(m_sceneEditor).lightingPasses().environment(); envMapProcessor != nullptr)
                    m_settings.ResetAccumulation |= envMapProcessor->debugGUI(layout.indent);
            }

            if (m_settings.UseNEE && m_settings.NEEType == 2)
            {
                RESET_ON_CHANGE(SettingsSliderFloat(
                    "Global Feedback", &m_settings.NEEAT_GlobalTemporalFeedbackWeight, 0.0f, 0.95f));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Blend of last-frame usage vs power-based sampling.\nSome power sampling is needed so new lights can appear.");

                RESET_ON_CHANGE(SettingsSliderFloat(
                    "Local / Global", &m_settings.NEEAT_LocalToGlobalSampleRatio, 0.0f, 0.95f));
                if (ImGui::IsItemHovered())
                {
                    const uint localCandidateSamples = ComputeCandidateSampleLocalCount(
                        m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
                    const uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(
                        m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
                    ImGui::SetTooltip(
                        "Share of NEE-AT candidates from local (tile) vs global samplers.\n"
                        "%d candidates: %d local, %d global",
                        m_settings.NEECandidateSamples, localCandidateSamples, globalCandidateSamples);
                }

                SettingsSliderFloat(
                    "Distant / Local",
                    &m_settings.NEEAT_Distant_vs_Local_Importance,
                    0.01f,
                    100.0f,
                    "%.2f",
                    ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Higher = more importance for envmap/sun vs local lights.");
            }

            if (ImGui::CollapsingHeader("Debugging"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                if (auto& lightSamplingCache = caustica::editor::requireWorldRenderer(m_sceneEditor).lightingPasses().lightSampling(); lightSamplingCache != nullptr)
                    m_settings.ResetAccumulation |= lightSamplingCache->debugGUI(layout.indent);
            }
        }
}

void EditorUI::BuildPathTracerPanel(const PanelLayout& layout)
{
        RAII_SCOPE(ImGui::PushID("PathTracerPanel");, ImGui::PopID(););
        if (ImGui::CollapsingHeader("Path Tracer", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

            int modeIndex = (m_settings.RealtimeMode)?(1):(0);
            if (SettingsCombo("Mode", &modeIndex, "Reference\0Realtime\0\0"))
            {
                const bool wasRealtimeMode = m_settings.RealtimeMode;
                m_settings.RealtimeMode = (modeIndex!=0);
                if (wasRealtimeMode != m_settings.RealtimeMode)
                {
                    m_settings.ResetAccumulation = true;
                    if (m_settings.RealtimeMode)
                        m_settings.ResetRealtimeCaches = true;
                }
            }

            SettingsCategoryHeader("Setup");
            {
                if (m_settings.RealtimeMode)
                {
                    {
                        UI_SCOPED_DISABLE( (m_settings.actualUseReSTIRDI() || m_settings.actualUseReSTIRGI() || m_settings.actualUseReSTIRPT()) );
                        SettingsInputInt("Samples / Pixel", &m_settings.RealtimeSamplesPerPixel);
                        m_settings.RealtimeSamplesPerPixel = dm::clamp(m_settings.RealtimeSamplesPerPixel, 1, 64);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                            ImGui::SetTooltip("How many full paths to trace per pixel from the primary surface\n(camera ray is not re-cast so there is no added AA)\n(currently incompatible with ReSTIR DI, ReSTIR GI & ReSTIR PT)");
                    }
                    if (ImGui::Button("Reset Realtime Caches", ImVec2(-1.f, 0.f)))
                        m_settings.ResetRealtimeCaches = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Reset temporal caches used by denoising and lighting.");
                }
                else
                {
                    RESET_ON_CHANGE(SettingsInputInt(
                        "Target Samples", &m_settings.AccumulationTarget));
                    m_settings.AccumulationTarget = dm::clamp(m_settings.AccumulationTarget, 1, 4 * 1024 * 1024); // this max is beyond float32 precision threshold; expect some banding creeping in when using more than 500k samples
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of path samples per pixel to collect");
                    ImGui::TextDisabled(
                        "Accumulated %d / %d  ·  %.3f ms",
                        caustica::accumulationSampleIndex(*m_sceneEditor.app()),
                        m_settings.AccumulationTarget,
                        caustica::avgTimePerFrame(*m_sceneEditor.app()) * 1000.0f);
                    if (ImGui::Button("Reset Accumulation", ImVec2(-1.f, 0.f)))
                        m_settings.ResetAccumulation = true;

                    RESET_ON_CHANGE(SettingsCheckbox(
                        "Pre-warm Caches", &m_settings.AccumulationPreWarmRealtimeCaches));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("If enabled, various lighting and etc systems will be pre-warmed before sample 0 is \naccumulated; otherwise they're reset and initial few samples will be lower quality.");

                    RESET_ON_CHANGE(SettingsCheckbox(
                        "Jitter Anti-aliasing", &m_settings.AccumulationAA));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each sample will have a random, per pixel jitter emulating box filter\nTODO: add option for Gaussian distribution for better AA");

                    m_settings.ResetRealtimeCaches |= m_settings.ResetAccumulation; // if there's a reset for any reason whilst we're in reference mode, reset the realtime caches too for determinism
                }

                RESET_ON_CHANGE(SettingsInputInt("Max Bounces", &m_settings.BounceCount));
                m_settings.BounceCount = dm::clamp(m_settings.BounceCount, 0, MAX_BOUNCE_COUNT);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max number of all bounces (including NEE and diffuse bounces)");
                RESET_ON_CHANGE(SettingsInputInt(
                    "Diffuse Bounces", &m_settings.DiffuseBounceCount));
                m_settings.DiffuseBounceCount = dm::clamp(m_settings.DiffuseBounceCount, 0, MAX_BOUNCE_COUNT);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max number of diffuse bounces (diffuse lobe and specular with roughness > 0.25 or similar depending on settings)");

                if (m_settings.RealtimeMode)
                {
                    RESET_ON_CHANGE(SettingsCheckbox(
                        "Firefly Filter", &m_settings.RealtimeFireflyFilterEnabled));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable smart firefly filter that clamps max radiance based on probability heuristic.");
                    if (m_settings.RealtimeFireflyFilterEnabled)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        RESET_ON_CHANGE(SettingsInputFloat(
                            "FF Threshold",
                            &m_settings.RealtimeFireflyFilterThreshold,
                            0.01f,
                            0.1f,
                            "%.5f"));
                        m_settings.RealtimeFireflyFilterThreshold = dm::clamp(m_settings.RealtimeFireflyFilterThreshold, 0.00001f, 1000.0f);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Better light importance sampling allows for setting higher firefly filter threshold and conversely.");
                        //ImGui::SameLine();
                        //RESET_ON_CHANGE( ImGui::Checkbox("RX", &m_settings.RealtimeFireflyFilterRelaxOnNonNoisy) ); 
                        //if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relax on non-noisy (direct, stable) radiance: clamp value will be FIREFLY_FILTER_RELAX_ON_NON_NOISY_K times bigger - helps with blooms");
                    }
                }
                else
                {
                    RESET_ON_CHANGE(SettingsCheckbox(
                        "Firefly Filter *", &m_settings.ReferenceFireflyFilterEnabled));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable smart firefly filter that clamps max radiance based on probability heuristic.\n* when both tonemapping autoexposure and firefly filter are enabled\nin reference mode, results are no longer deterministic!");
                    if (m_settings.ReferenceFireflyFilterEnabled)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                        RESET_ON_CHANGE(SettingsInputFloat(
                            "FF Threshold",
                            &m_settings.ReferenceFireflyFilterThreshold,
                            0.1f,
                            0.2f,
                            "%.5f"));
                        m_settings.ReferenceFireflyFilterThreshold = dm::clamp(m_settings.ReferenceFireflyFilterThreshold, 0.01f, 1000.0f);
                    }
                }

                RESET_ON_CHANGE(SettingsInputFloat(
                    "Texture MIP Bias", &m_settings.TexLODBias));

                RESET_ON_CHANGE(SettingsInputInt(
                    "Environment MIP", &m_settings.EnvironmentMapDiffuseSampleMIPLevel));
                m_settings.EnvironmentMapDiffuseSampleMIPLevel = dm::clamp(m_settings.EnvironmentMapDiffuseSampleMIPLevel, 0, 16);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the specific MIP level to sample environment map texture during light sampling and for main path terminating\ninto sky after a diffuse scatter. Only 0 produces unbiased results.");

                RESET_ON_CHANGE(SettingsCheckbox(
                    "Russian Roulette", &m_settings.EnableRussianRoulette));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables stochastic path termination for low throughput diffuse paths");
            }

            SettingsCategoryHeader("Post Processing");
            {
                if (m_settings.RealtimeMode)
                {
#if CAUSTICA_WITH_ANY_DLSS
                    const bool dlssAvailable = m_settings.IsDLSSSuported;
                    const bool dlssRRAvailable = m_settings.IsDLSSRRSupported; 
#else
                    const bool dlssAvailable = false;
                    const bool dlssRRAvailable = false;
#endif
                    const char* items[] = { "Disabled", "TAA", "DLSS", "DLSS-RR" };

                    const int itemCount = IM_ARRAYSIZE(items);

                    m_settings.RealtimeAA = dm::clamp(m_settings.RealtimeAA, 0, dlssAvailable ? itemCount : 1);

                    if (SettingsBeginCombo("AA / SR / Denoise", items[m_settings.RealtimeAA]))
                    {
                        for (int i = 0; i < itemCount; i++)
                        {
                            bool enabled = false;
                            enabled |= i <2;
                            enabled |= (i == 2) && dlssAvailable;
                            enabled |= (i == 3) && dlssRRAvailable;
                            UI_SCOPED_DISABLE(!enabled);

                            bool isSelected = (m_settings.RealtimeAA == i);
                            if (ImGui::Selectable(items[i], isSelected))
                                m_settings.RealtimeAA = i;
                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                        SettingsEndCombo();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(
                        "TAA        - generic temporal anti-aliasing\n"
                        "DLSS       - Nvidia Deep Learning Super Sampling (lower resolution render + upscale)\n"
                        "DLAA       - Nvidia Deep Learning Anti Aliasing (full resolution render)\n"
                        "DLSS-RR    - DLSS + Ray Reconstruction (lower resolution render + denoise & upscale)\n"
                        "\nIndividual DLSS options available under global `DLSS` options"
                    );

#if CAUSTICA_WITH_ANY_DLSS
                    if (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3)
                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent); ImGui::PushID("PPDLSSQual");,  ImGui::Unindent(layout.indent); ImGui::PopID(););
                        m_settings.DLSSMode = DLSSModeUI(m_settings.DLSSMode);
                    }
#endif

                    {
                        UI_SCOPED_DISABLE(!m_settings.RealtimeMode || m_settings.RealtimeAA==3);
                        bool notTrue = false;
                        SettingsCheckbox(
                            "Standalone NRD",
                            (m_settings.RealtimeAA == 3)
                                ? &notTrue
                                : &m_settings.StandaloneDenoiser);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Enables NVIDIA Real-Time Denoisers (NRD) that execute before TAA/DLSS/DLAA pass\nNote: no built-in denoiser available in 'Reference' mode, however \n'Photo mode screenshot' button launches external denoiser!");
                    }
                }
                else // !m_settings.RealtimeMode
                {
#if CAUSTICA_WITH_OIDN
                    bool oidnChanged = false;
                    oidnChanged |= SettingsCheckbox(
                        "OIDN Denoiser", &m_settings.ReferenceOIDNDenoiser);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs Intel Open Image denoise once after the Reference accumulation target is reached.\nThe denoised HDR result is reused until accumulation is reset.");

                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                        UI_SCOPED_DISABLE(!m_settings.ReferenceOIDNDenoiser);

                        oidnChanged |= SettingsCheckbox(
                            "Use GPU", &m_settings.ReferenceOIDNUseGPU);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Uses OIDN GPU denoising when a supported CUDA/HIP/SYCL device is available; otherwise falls back to CPU.");

                        UI_SCOPED_DISABLE(true);
                        SettingsCombo(
                            "Denoiser",
                            &m_settings.ReferenceOIDNDenoiserType,
                            "OpenImageDenoise\0\0");
                    }

                    {
                        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                        UI_SCOPED_DISABLE(!m_settings.ReferenceOIDNDenoiser);

                        oidnChanged |= SettingsCombo(
                            "Guide Passes",
                            &m_settings.ReferenceOIDNPasses,
                            "Color Only\0Albedo\0Albedo + Normal\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Selects which auxiliary OIDN guide passes are used when available.");

                        oidnChanged |= SettingsCombo(
                            "Prefilter",
                            &m_settings.ReferenceOIDNPrefilter,
                            "None\0Fast\0Accurate\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Prefilters noisy auxiliary guide passes before beauty denoising.");

                        oidnChanged |= SettingsCombo(
                            "Quality",
                            &m_settings.ReferenceOIDNQuality,
                            "Fast\0Balanced\0High\0\0");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("OIDN quality/performance mode.");
                    }

                    if (oidnChanged)
                        m_settings.ReferenceOIDNDenoiserChanged = true;
#else
                    {
                        bool oidnDisabled = false;
                        UI_SCOPED_DISABLE(true);
                        SettingsCheckbox("OIDN Denoiser", &oidnDisabled);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("OIDN support is disabled in this build. Enable CAUSTICA_WITH_OIDN in CMake.");
#endif

                    if (ImGui::Button("Photo Mode Screenshot", ImVec2(-1.f, 0.f)))
                        m_editorUI.ExperimentalPhotoModeScreenshot = true;
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
                    SettingsCheckbox("Tone Mapping", &m_settings.EnableToneMapping);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Full tone mapping settings available under global `Tone Mapping` options");
                }
            }

            SettingsCategoryHeader("Light Sampling");
            {
                if (m_settings.RealtimeMode)
                {
                    if (m_settings.UseReSTIRGI && m_settings.UseReSTIRPT)
                        m_settings.UseReSTIRGI = false;

                    {
                        bool nullCheckbox = false;
                        bool disabled = !m_settings.UseNEE || (m_settings.RealtimeAA==3 && m_settings.DisableReSTIRsWithDLSSRR);
                        UI_SCOPED_DISABLE(disabled);
                        RESET_ON_CHANGE(SettingsCheckbox(
                            "ReSTIR DI", (disabled) ? &nullCheckbox : &m_settings.UseReSTIRDI));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_settings.DisableReSTIRsWithDLSSRR = !m_settings.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR DI (RTXDI) requires Next Event Estimation to be enabled\nand this implementation is currently not tuned to work with DLSS-RR");

                    {
                        bool nullCheckbox = false;
                        bool disabled = m_settings.RealtimeAA==3 && m_settings.DisableReSTIRsWithDLSSRR;
                        UI_SCOPED_DISABLE( disabled );
                        const bool changed = SettingsCheckbox(
                            "ReSTIR GI", (disabled) ? &nullCheckbox : &m_settings.UseReSTIRGI);
                        RESET_ON_CHANGE(changed);
                        if (changed && !disabled && m_settings.UseReSTIRGI)
                            m_settings.UseReSTIRPT = false;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_settings.DisableReSTIRsWithDLSSRR = !m_settings.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR GI and ReSTIR PT are mutually exclusive.\nReSTIR GI is currently not tuned to work well with DLSS-RR\nUse middle mouse button to enable anyway");

                    {
                        bool nullCheckbox = false;
                        bool disabled = m_settings.RealtimeAA==3 && m_settings.DisableReSTIRsWithDLSSRR;
                        UI_SCOPED_DISABLE(disabled);
                        const bool changed = SettingsCheckbox(
                            "ReSTIR PT", (disabled) ? &nullCheckbox : &m_settings.UseReSTIRPT);
                        RESET_ON_CHANGE(changed);
                        if (changed && !disabled && m_settings.UseReSTIRPT)
                            m_settings.UseReSTIRGI = false;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            m_settings.DisableReSTIRsWithDLSSRR = !m_settings.DisableReSTIRsWithDLSSRR;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("ReSTIR PT and ReSTIR GI are mutually exclusive.\nReSTIR PT is currently not tuned to work well with DLSS-RR\nUse middle mouse button to enable anyway");

                    if (SettingsCombo("DI / GI Preset", (int*)&m_settings.RTXDIRestirPreset,
                        "(Custom)\0Fast\0Medium\0Unbiased\0Ultra\0Reference\0\0"))
                    {
                        m_settings.applyRTXDIRestirPreset();
                    }
                    if (SettingsCombo("PT Preset", (int*)&m_settings.RTXDIRestirPTPreset,
                        "(Custom)\0Fast\0Medium\0Ultra\0\0"))
                    {
                        m_settings.applyRTXDIRestirPTPreset();
                    }
                }

                RESET_ON_CHANGE(SettingsCheckbox(
                    "Next Event Estimation", &m_settings.UseNEE));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables NEE a.k.a. direct light importance sampling (this includes ReSTIR DI but not ReSTIR GI)\nNote: analytic lights currently only come out of NEE so they will be missing when NEE is disabled");

                if (m_settings.UseNEE)
                {
                    SettingsCategoryHeader("NEE Settings");
                    {
                        RESET_ON_CHANGE(SettingsCombo(
                            "Sampling",
                            (int*)&m_settings.NEEType,
                            "Uniform\0Power+\0NEE-AT\0\0"));
                        m_settings.NEEType = dm::clamp(m_settings.NEEType, 0, 2);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light importance sampling technique to use for NEE.\nNote: Additional NEE-AT settings are exposed in 'Lighting -> NEE-AT' UI section.");
    
                        RESET_ON_CHANGE(SettingsInputInt(
                            "Candidate Samples", &m_settings.NEECandidateSamples, 1));
                        if (ImGui::IsItemHovered()) 
                        {
                            if (m_settings.NEEType != 2)
                                ImGui::SetTooltip("This is the number of light samples weighted with BSDF used to pick each full sample.");
                            else
                            {
                                uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
                                uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);

                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is the number of light samples weighted with BSDF used to pick each full sample.\n"
                                                                              "Out of those %d will be Local and %d will be Global NEE-AT samples", localCandidateSamples, globalCandidateSamples);
                            }
                        }
                        m_settings.NEECandidateSamples = dm::clamp(m_settings.NEECandidateSamples, 1, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);

                        RESET_ON_CHANGE(SettingsInputInt(
                            "Full Samples", &m_settings.NEEFullSamples, 1));
                        m_settings.NEEFullSamples = dm::clamp(m_settings.NEEFullSamples, 0, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is the number of light samples to shadow test and integrate\nNote: Maximum total number of samples is 63");

                        RESET_ON_CHANGE(SettingsCombo(
                            "MIS Type",
                            (int*)&m_settings.NEEMISType,
                            "Full\0ApproxInRealtime\0Approximate\0\0"));
                        m_settings.NEEMISType = dm::clamp(m_settings.NEEMISType, 0, 2);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path (BSDF) vs light sampler Multiple Importance Sampling approach.\n'Approximate' is faster and easier to implement but more noisy, with \nthe impact of noise especially detrimental in reference accumulation.");
                    }
                }
            }

            if (ImGui::CollapsingHeader("PT: Advanced settings", 0))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
                SettingsCategoryHeader("Features");
                SettingsCombo(
                    "Nested Dielectrics",
                    (int*)&m_settings.NestedDielectricsQuality,
                    "Off\0Fast\0Quality\0");
                m_settings.NestedDielectricsQuality = clamp( m_settings.NestedDielectricsQuality, 0, 2 );
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Priority-based nested dielectrics; 'Quality' allows for more \ncorrect rejections, 'Fast' is.. well, faster.");
                if (m_settings.RealtimeAA == 3)
                {
                    RESET_ON_CHANGE(SettingsInputFloat(
                        "RR Brightness Clamp", &m_settings.DLSSRRBrightnessClampK));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("RR doesn't handle too bright (relatively) areas, causing unwanted noise;\nEnabling this will clamp brightness at the expense of bloom.\nTODO: replace this with local tonemap or similar");
                }

                SettingsCategoryHeader("Performance");
                {
                    if (m_NVAPI_SERSupported)
                    {
                        RESET_ON_CHANGE(SettingsCheckbox(
                            "NVAPI HitObject", &m_settings.NVAPIHitObjectExtension)); // <- while there's no need to reset accumulation since this is a performance only feature, leaving the reset in for testing correctness
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("If disabled, traditional TraceRay path is used.\nIf enabled, TraceRayInline->MakeHit->ReorderThread->InvokeHit approach is used!");
                        if (m_settings.NVAPIHitObjectExtension)
                        {
                            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                            SettingsCheckbox(
                                "Reorder Threads", &m_settings.NVAPIReorderThreads);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This enables/disables the actual ReorderThread call in the shader.");
                        }
                        if (m_settings.NVAPIHitObjectExtension)
                            m_settings.DXHitObjectExtension = false;
                    }
                    else
                    {
                        ImGui::Text("<NVAPI Hit Object Extension not supported>");
                        m_settings.NVAPIHitObjectExtension = false;
                    }

#if CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619
                    if (getDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
                    {
                        RESET_ON_CHANGE(SettingsCheckbox(
                            "DX HitObject", &m_settings.DXHitObjectExtension));
                        if (m_settings.DXHitObjectExtension)
                        {
                            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                            RESET_ON_CHANGE(SettingsCheckbox(
                                "Maybe Reorder", &m_settings.DXMaybeReorderThreads));
                        }
                        if (m_settings.DXHitObjectExtension)
                            m_settings.NVAPIHitObjectExtension = false;
                    }
#endif
          
                    RESET_ON_CHANGE(SettingsCheckbox(
                        "Explicit FP16", &m_settings.UseFp16Types));

                    RESET_ON_CHANGE(SettingsCheckbox(
                        "LD BSDF Sampler", &m_settings.EnableLDSamplerForBSDF));
                }
            }
        }
}


} // namespace caustica::editor

