#include "ui/EditorUIInternal.h"
#include "PassDebugGui.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/CameraApi.h>
#include <core/task/TaskRuntime.h>
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneCameraAccess.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>
#include <platform/file_dialog.h>
#include <core/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

namespace
{
// Scene cameras may carry glTF exposure / tone-map metadata. The camera update
// system applies that metadata every frame, so mirror editor overrides back to
// the active camera rather than letting the next update undo the UI change.
void syncToneMappingToActiveSceneCamera(SceneEditor& sceneEditor, const ToneMappingParameters& params)
{
    auto& app = editorApp(sceneEditor);
    const uint32_t selectedCamera = caustica::selectedCameraIndex(app);
    if (selectedCamera == 0)
        return; // Free camera: PathTracerSettings is the authoritative source.

    const auto& cameras = caustica::sceneCameraEntities(app);
    const size_t index = size_t(selectedCamera - 1);
    auto* entityWorld = caustica::entityWorld(app);
    if (!entityWorld || index >= cameras.size())
        return;

    auto* camera = caustica::scene::tryGetCamera(entityWorld->world(), cameras[index]);
    auto* perspective = camera ? caustica::scene::tryGetPerspectiveCameraData(*camera) : nullptr;
    if (!perspective)
        return;

    perspective->enableAutoExposure = params.autoExposure;
    perspective->toneMapOperator = toneMapOperatorId(params.toneMapOperator);
    perspective->exposureCompensation = params.exposureCompensation;
    perspective->exposureValue = params.exposureValue;
    perspective->exposureValueMin = params.exposureValueMin;
    perspective->exposureValueMax = params.exposureValueMax;
}
} // namespace

void EditorUI::BuildOpacityMicroMapsPanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Opacity Micro-Maps", ImGuiTreeNodeFlags_DefaultOpen))
        {
            UI_SCOPED_INDENT(layout.indent);

            if (auto opacityMicromapBuilder = caustica::opacityMicromapBuilder(editorApp(m_sceneEditor)); opacityMicromapBuilder)
            {
                if (const auto* renderData = caustica::latestPublishedRenderData(editorApp(m_sceneEditor)))
                    m_settings.ResetAccumulation |= DrawOpacityMicromapDebug(
                        *opacityMicromapBuilder,
                        *renderData,
                        layout.indent);
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
        bool toneMappingChanged = false;
        toneMappingChanged |= SettingsCheckbox("Enabled##ToneMapping", &m_settings.EnableToneMapping);

        const std::string currentOperator =
            tonemapOperatorToString.at(m_settings.ToneMappingParams.toneMapOperator);
        if (SettingsBeginCombo("Operator", currentOperator.c_str()))
        {
            for (auto it = tonemapOperatorToString.begin(); it != tonemapOperatorToString.end(); ++it)
            {
                const bool is_selected = it->first == m_settings.ToneMappingParams.toneMapOperator;
                if (ImGui::Selectable(it->second.c_str(), is_selected))
                {
                    m_settings.ToneMappingParams.toneMapOperator = it->first;
                    toneMappingChanged = true;
                }
            }
            SettingsEndCombo();
        }

        SettingsCategoryHeader("Exposure");
        toneMappingChanged |= SettingsCheckbox("Auto Exposure", &m_settings.ToneMappingParams.autoExposure);

        if (m_settings.ToneMappingParams.autoExposure)
        {
            toneMappingChanged |= SettingsInputFloat("Minimum EV", &m_settings.ToneMappingParams.exposureValueMin);
            m_settings.ToneMappingParams.exposureValueMin = dm::min(
                m_settings.ToneMappingParams.exposureValueMax,
                m_settings.ToneMappingParams.exposureValueMin);
            toneMappingChanged |= SettingsInputFloat("Maximum EV", &m_settings.ToneMappingParams.exposureValueMax);
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
                {
                    m_settings.ToneMappingParams.exposureMode = it->first;
                    toneMappingChanged = true;
                }
            }
            SettingsEndCombo();
        }

        toneMappingChanged |= SettingsInputFloat("Compensation", &m_settings.ToneMappingParams.exposureCompensation);
        m_settings.ToneMappingParams.exposureCompensation =
            dm::clamp(m_settings.ToneMappingParams.exposureCompensation, -12.0f, 12.0f);

        toneMappingChanged |= SettingsInputFloat("Exposure Value", &m_settings.ToneMappingParams.exposureValue);
        m_settings.ToneMappingParams.exposureValue = dm::clamp(
            m_settings.ToneMappingParams.exposureValue,
            dm::log2f(0.1f * 0.1f * 0.1f),
            dm::log2f(100000.f * 100.f * 100.f));

        toneMappingChanged |= SettingsInputFloat("Film Speed", &m_settings.ToneMappingParams.filmSpeed);
        m_settings.ToneMappingParams.filmSpeed =
            dm::clamp(m_settings.ToneMappingParams.filmSpeed, 1.0f, 6400.0f);

        toneMappingChanged |= SettingsInputFloat("F-Number", &m_settings.ToneMappingParams.fNumber);
        m_settings.ToneMappingParams.fNumber =
            dm::clamp(m_settings.ToneMappingParams.fNumber, 0.1f, 100.0f);

        toneMappingChanged |= SettingsInputFloat("Shutter", &m_settings.ToneMappingParams.shutter);
        m_settings.ToneMappingParams.shutter =
            dm::clamp(m_settings.ToneMappingParams.shutter, 0.1f, 10000.0f);

        SettingsCategoryHeader("White Balance");
        toneMappingChanged |= SettingsCheckbox("Enabled##WhiteBalance", &m_settings.ToneMappingParams.whiteBalance);

        toneMappingChanged |= SettingsInputFloat("White Point", &m_settings.ToneMappingParams.whitePoint);
        m_settings.ToneMappingParams.whitePoint =
            dm::clamp(m_settings.ToneMappingParams.whitePoint, 1905.0f, 25000.0f);

        toneMappingChanged |= SettingsInputFloat("Max Luminance", &m_settings.ToneMappingParams.whiteMaxLuminance);
        m_settings.ToneMappingParams.whiteMaxLuminance =
            dm::clamp(m_settings.ToneMappingParams.whiteMaxLuminance, 0.1f, FLT_MAX);

        toneMappingChanged |= SettingsInputFloat("White Scale", &m_settings.ToneMappingParams.whiteScale);
        m_settings.ToneMappingParams.whiteScale =
            dm::clamp(m_settings.ToneMappingParams.whiteScale, 0.f, 100.f);

        toneMappingChanged |= SettingsCheckbox("Clamp", &m_settings.ToneMappingParams.clamped);

        SettingsCategoryHeader("Camera LUT");
        toneMappingChanged |= SettingsCheckbox("Enabled##CameraLut", &m_settings.ToneMappingParams.cameraLutEnabled);
        toneMappingChanged |= SettingsCheckbox("After Tone Map", &m_settings.ToneMappingParams.cameraLutAfterToneMap);
        const std::string currentPreset =
            CameraLutPresetToString.at(m_settings.ToneMappingParams.cameraLutPreset);
        if (SettingsBeginCombo("Preset", currentPreset.c_str()))
        {
            for (const auto& preset : CameraLutPresetToString)
            {
                if (preset.first == CameraLutPreset::CustomFile)
                    continue;
                const bool selected = preset.first == m_settings.ToneMappingParams.cameraLutPreset;
                if (ImGui::Selectable(preset.second.c_str(), selected))
                {
                    m_settings.ToneMappingParams.applyCameraLutPreset(preset.first);
                    toneMappingChanged = true;
                }
            }
            SettingsEndCombo();
        }
        if (ImGui::Button("Load 1D / 3D .cube##CameraLut"))
        {
            std::string picked = m_settings.ToneMappingParams.cameraLutPath;
            if (::caustica::FileDialog(true, "Cube LUT (*.cube)\0*.cube\0All files\0*.*\0", picked))
            {
                std::string error;
                if (!m_settings.ToneMappingParams.loadCameraLut(picked, &error))
                    caustica::warning("Failed to load camera LUT '%s': %s", picked.c_str(), error.c_str());
                else
                    toneMappingChanged = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear##CameraLut"))
        {
            m_settings.ToneMappingParams.clearCameraLut();
            toneMappingChanged = true;
        }
        if (!m_settings.ToneMappingParams.cameraLutPath.empty())
            ImGui::TextWrapped("%s", m_settings.ToneMappingParams.cameraLutPath.c_str());

        if (toneMappingChanged)
        {
            syncToneMappingToActiveSceneCamera(m_sceneEditor, m_settings.ToneMappingParams);
            m_settings.ResetAccumulation = true;
        }
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

            if (ImGui::CollapsingHeader("TaskRuntime / LoadSession"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                const task::RuntimeStats stats = task::snapshotStats();
                ImGui::Text("Workers: Any %u  IO %u", stats.workerCount, stats.ioWorkerCount);
                ImGui::Text("Queued: Any %u  Logic %u  Render %u  IO %u",
                    stats.anyQueued, stats.logicQueued, stats.renderQueued, stats.ioQueued);
                ImGui::Text("Generation: Frame %llu  Load %llu",
                    (unsigned long long)stats.frameGeneration,
                    (unsigned long long)stats.loadGeneration);

                const caustica::SceneLoadStatus load = caustica::sceneLoadStatus(editorApp(m_sceneEditor));
                ImGui::Text("LoadSession: %s  busy=%s  progress=%d%%",
                    load.phaseName,
                    load.busy ? "yes" : "no",
                    load.progressPercent);
                if (load.gpuStreaming)
                {
                    ImGui::Text("  stream step=%u  textures rem=%zu  meshes %zu/%zu  inFlight=%s",
                        load.streamStep,
                        load.texturesRemaining,
                        load.meshBegin, load.meshTotal,
                        load.stepInFlight ? "yes" : "no");
                }
            }

            if (ImGui::CollapsingHeader("Frame telemetry"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                const render::FrameTelemetry& telemetry =
                    m_sceneEditor.diagnostics().frameTelemetry;
                const render::FrameTelemetrySample frame = telemetry.latestRendered();
                const render::FrameTelemetrySample gpuFrame = telemetry.latestGpu();
                render::FrameTelemetrySample gpuPassFrame = telemetry.latestGpuPasses();
                if (!frame.valid)
                {
                    ImGui::TextDisabled("Waiting for the first frame...");
                }
                else
                {
                    ImGui::Text("CPU/render frame %u", frame.frameIndex);
                    if (gpuFrame.valid)
                        ImGui::Text("GPU frame %u  %.3f ms", gpuFrame.frameIndex, gpuFrame.gpuMs);
                    else
                        ImGui::Text("GPU pending");
                    ImGui::Text("Logic %.3f  Extract %.3f  QueueWait %.3f ms",
                        frame.cpu(render::FrameCpuStage::Logic),
                        frame.cpu(render::FrameCpuStage::Extract),
                        frame.cpu(render::FrameCpuStage::FrameQueueWait));
                    ImGui::Text("Render %.3f  Acquire %.3f  Present %.3f ms",
                        frame.cpu(render::FrameCpuStage::Render),
                        frame.cpu(render::FrameCpuStage::Acquire),
                        frame.cpu(render::FrameCpuStage::Present));
                    ImGui::Text("Build %.3f  Compile %.3f  Record %.3f ms",
                        frame.cpu(render::FrameCpuStage::GraphBuild),
                        frame.cpu(render::FrameCpuStage::GraphCompile),
                        frame.cpu(render::FrameCpuStage::CommandRecord));
                    ImGui::Text("Present: %s  requested vsync=%s  active vsync=%s",
                        frame.presentHeadless ? "headless" : (frame.presentWindowed ? "windowed" : "fullscreen"),
                        frame.presentRequestedVsync ? "on" : "off",
                        frame.presentActiveVsync ? "on" : "off");
                    ImGui::Text("Tearing: supported=%s  active=%s  back buffers=%u",
                        frame.presentTearingSupported ? "yes" : "no",
                        frame.presentTearingActive ? "yes" : "no",
                        frame.presentBackBufferCount);
                    ImGui::Text("Graph: %u passes  %u waves  %u parallel batches  plan cache %s",
                        frame.graphPasses,
                        frame.graphWaves,
                        frame.parallelBatches,
                        frame.graphPlanCacheHit ? "hit" : "miss");
                    if (gpuPassFrame.gpuPassesValid)
                    {
                        std::sort(
                            gpuPassFrame.gpuPasses.begin(),
                            gpuPassFrame.gpuPasses.end(),
                            [](const render::FrameGpuPassTiming& a, const render::FrameGpuPassTiming& b) {
                                return a.milliseconds > b.milliseconds;
                            });
                        ImGui::Text("GPU passes, frame %u", gpuPassFrame.frameIndex);
                        const size_t visiblePasses = std::min<size_t>(gpuPassFrame.gpuPasses.size(), 12);
                        for (size_t i = 0; i < visiblePasses; ++i)
                        {
                            const render::FrameGpuPassTiming& pass = gpuPassFrame.gpuPasses[i];
                            ImGui::Text("  %-32s %7.3f ms", pass.name.c_str(), pass.milliseconds);
                        }
                    }
                }

                ImGui::Separator();
                ImGui::Checkbox("Parallel graph recording", &m_settings.ParallelRenderGraphRecording);
                ImGui::InputInt("Minimum parallel cost",
                    &m_settings.RenderGraphMinParallelRecordingCost, 1, 4);
                m_settings.RenderGraphMinParallelRecordingCost =
                    std::max(1, m_settings.RenderGraphMinParallelRecordingCost);
                ImGui::InputInt("Maximum recording jobs (0 = auto)",
                    &m_settings.RenderGraphMaxRecordingJobs, 1, 4);
                m_settings.RenderGraphMaxRecordingJobs =
                    std::max(0, m_settings.RenderGraphMaxRecordingJobs);
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
                DrawZoomToolDebug(*m_sceneEditor.zoomTool());
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
