#include "PassDebugGui.h"

#include "ui/ui_macros.h"

#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/lighting/distant/ProceduralSky.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/debug/ZoomTool.h>
#include <render/omm/GpuBakeRhi.h>
#include <scene/SceneRenderData.h>
#include <shaders/PathTracer/Config.h>
#include <core/path_utils.h>
#include <core/scope.h>
#include <platform/file_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace caustica::math;

namespace caustica::editor
{
namespace
{

float WrapDegrees360(float deg)
{
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f)
        deg += 360.0f;
    return deg;
}

std::string CubeResToString(uint res)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%u x %u x 6", res, res);
    return buf;
}

} // namespace

bool DrawLightSamplingInfo(LightSamplingCache& cache)
{
    RAII_SCOPE(ImGui::PushID("LightSamplingCacheInfoGUI");, ImGui::PopID(););

    const LightSamplingCache::LightCounts counts = cache.lightCounts();
    if (counts.total >= CAUSTICA_LIGHTING_MAX_LIGHTS)
    {
        ImGui::TextColored({ 1, 0.5f, 0.5f, 1 },
            "Light count overflow — raise CAUSTICA_LIGHTING_MAX_LIGHTS (%d)",
            CAUSTICA_LIGHTING_MAX_LIGHTS);
    }
    else
    {
        ImGui::Text("Lights: %u  (env %u / tri %u / analytic %u)",
            counts.total, counts.envmap, counts.triangle, counts.analytic);
    }

    return false;
}

bool DrawLightSamplingDebug(LightSamplingCache& cache, float indent)
{
    RAII_SCOPE(ImGui::PushID("LightSamplingCacheDebugGUI");, ImGui::PopID(););

    ImGui::Checkbox("Debug draw all lights", &cache.debugDrawLights());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wireframe colour indicates type: red - environment map; green - emissive triangles; blue - analytic.");

    ImGui::Checkbox("Debug draw NEE-AT tile light connections", &cache.debugDrawTileLightConnections());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows lights sampled by a specific tile local sampling pdf");

    ImGui::Checkbox("Freeze NEE-AT feedback updates", &cache.freezeUpdates());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Feedback from the path tracer will remain frozen while this option is enabled.");

    const char* debugOptions =
        "Disabled\0Disocclusion\0NoHistoryFeedback\0MissingFeedbackScreenSpaceCoherent\0"
        "MissingFeedbackWorldSpaceCoherent\0FeedbackRawScreenSpaceCoherent\0"
        "FeedbackRawWorldSpaceCoherent\0LowResBlendedFeedback\0FeedbackAfterClear\0"
        "TileHeatmap\0ValidateCorrectness\0\0";
    ImGui::Combo("NEE-AT debug view", (int*)&cache.debugDrawType(), debugOptions);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show various NEE-AT buffers");

    ImGui::Checkbox("Debug disable local tile jitter", &cache.debugDisableJitter());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mapping from pixels to tiles will be jittered to avoid denoising artifacts.\nIt also helps with spatial sharing.\nDisable for debugging.");

    ImGui::Checkbox("Debug disable last frame feedback", &cache.debugDisableLastFrameFeedback());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Simply disables last frame's feedback for debugging/validation.\nQuality should revert to slightly worse than power based sampling.");

    ImGui::Checkbox("Debug freeze frustum updates", &cache.freezeFrustumUpdates());

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced settings"))
    {
        ImGui::SliderFloat("ScreenSpaceVsWorldSpaceThreshold", &cache.screenSpaceVsWorldSpaceThreshold(), 0.02f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Used to determine, for each sampling location, whether it's more optimal to use screen tiles or world voxels for caching.");

        ImGui::SliderFloat("ReservoirHistoryDropoff", &cache.reservoirHistoryDropoff(), 0.0f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The amount of history sharing from past and from neighbours. Some is useful, \ntoo much will add lag and allow strong lights to dwarf out others.");

        ImGui::SliderFloat("DepthDisocclusionThreshold", &cache.depthDisocclusionThreshold(), 0.999f, 20.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("During motion reprojection, drop samples if really far from target");

        ImGui::Checkbox("Sample environment proxy lights", &cache.sampleBakedEnvironmentToggle());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("If enabled, environment map texture will not be sampled directly by NEE\nbut will be baked into sampling proxies like emissive triangles.\nBiased, faster but more blurry shadows in some cases.");

        ImGui::Text("Importance boosts:");
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        ImGui::Checkbox("...by light intensity change", &cache.importanceBoostIntensityDelta());
        {
            RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
            RAII_SCOPE(ImGui::PushID("Delta");, ImGui::PopID(););
            ImGui::InputFloat("multiplier", &cache.importanceBoostIntensityDeltaMul());
            cache.importanceBoostIntensityDeltaMul() = math::clamp(cache.importanceBoostIntensityDeltaMul(), 0.0f, 1000.0f);
        }
        ImGui::Checkbox("...by light frustum proximity", &cache.importanceBoostFrustum());
        {
            RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
            RAII_SCOPE(ImGui::PushID("FrustProx");, ImGui::PopID(););
            ImGui::InputFloat("multiplier", &cache.importanceBoostFrustumMul());
            cache.importanceBoostFrustumMul() = math::clamp(cache.importanceBoostFrustumMul(), 0.0f, 1000.0f);
            ImGui::InputFloat("fade distance", &cache.importanceBoostFrustumFadeDistance());
            cache.importanceBoostFrustumFadeDistance() = math::clamp(cache.importanceBoostFrustumFadeDistance(), 0.0f, 1000.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How fast the boost fades outside of the frustum\nThe bigger the value, the slower it fades");
        }
        ImGui::Checkbox("...by pre-filter merge", &cache.importanceBoostPreFilter());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Will allow stronger feedback in 3x3 kernel to 'overwhelm' neighbors\nEXPERIMENTAL - SUPER-SLOW");
    }

    return false;
}

bool DrawEnvMapProcessorDebug(EnvMapProcessor& env, float indent)
{
    (void)indent;
    bool resetAccumulation = false;
    auto mark = [&](bool v) { resetAccumulation |= v; };

    std::string currentRes = CubeResToString(env.targetCubeResolutionRaw());
    if (ImGui::BeginCombo("Target cube res", currentRes.c_str()))
    {
        const uint resolutions[] = { 512, 1024, 2048, 4096 };
        for (uint resolution : resolutions)
        {
            std::string itemName = CubeResToString(resolution);
            const bool selected = itemName == currentRes;
            if (ImGui::Selectable(itemName.c_str(), selected))
                env.setTargetCubeResolution(resolution);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
        resetAccumulation = true;
    }

    mark(ImGui::Checkbox("Force dynamic", &env.forceDynamicBake()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Force re-generate every frame even if static (for performance testing only)");

    if (env.bc6uCompressionEnabled())
    {
        int quality = env.compressionQuality();
        if (ImGui::Combo("BC6U compression", &quality, "Off\0Fast\0Quality\0\0"))
        {
            env.setCompressionQuality(quality);
            resetAccumulation = true;
        }
    }
    else
    {
        ImGui::Text("BC6U compression not currently supported in Vulkan");
    }

    if (ImGui::Button("Save baked cubemap"))
    {
        std::string fileName;
        if (caustica::FileDialog(false, "DDS files\0*.dds\0\0", fileName))
            env.requestSaveBakedCubemap(std::move(fileName));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save baked cubemap. It will be rebaked with EnvMapRadianceScale set to 1.0 before saving.");

    return resetAccumulation;
}

bool DrawProceduralSkyDebug(ProceduralSky& sky, float indent)
{
    bool changed = false;
    auto mark = [&](bool v) { changed |= v; };

    ImGui::TextWrapped("Hillaire 2020 Sky Atmosphere — bake into dynamic environment cubemap.");
    RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););

    if (ImGui::CollapsingHeader("Sun Direction", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););

        const bool namedPreset = caustica::isProceduralSky(sky.activePresetType().c_str())
            && sky.activePresetType() != caustica::c_EnvMapProcSky;
        if (namedPreset)
        {
            ImGui::TextWrapped(
                "A named sky preset is driving the sun direction.\n"
                "Switch Environment Override to 'sky (manual)' for free elevation/azimuth control.");
        }

        ImGui::BeginDisabled(namedPreset);

        mark(ImGui::Checkbox("animate sun (day cycle)", &sky.animateSun()));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, elevation/azimuth follow a day arc.\nDisable for full manual control.");

        if (sky.animateSun())
        {
            mark(ImGui::SliderFloat("Day speed (cycles / min)", &sky.sunAnimSpeed(), 0.0f, 30.0f, "%.2f"));
            mark(ImGui::SliderFloat("Max elevation (deg)", &sky.sunAnimMaxElevation(), 5.0f, 89.0f, "%.1f"));
            mark(ImGui::SliderFloat("Noon azimuth (deg)", &sky.noonAzimuthDeg(), 0.0f, 360.0f, "%.1f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Azimuth at solar noon. Day arc swings ±90° around this bearing.");
            sky.noonAzimuthDeg() = WrapDegrees360(sky.noonAzimuthDeg());
            mark(ImGui::SliderFloat("Day phase", &sky.sunAnimPhase(), 0.0f, 1.0f, "%.3f"));
        }
        else
        {
            mark(ImGui::SliderFloat("Elevation (deg)", &sky.sunElevationDeg(), -20.0f, 89.0f, "%.2f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = horizon, 90 = zenith, negative = below horizon.");
            mark(ImGui::SliderFloat("Azimuth (deg)", &sky.sunAzimuthDeg(), 0.0f, 360.0f, "%.2f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = +X, 90 = +Y in sky-local (Z-up) space.");
            mark(ImGui::InputFloat("Elevation##in", &sky.sunElevationDeg(), 1.0f, 5.0f, "%.2f"));
            mark(ImGui::InputFloat("Azimuth##in", &sky.sunAzimuthDeg(), 1.0f, 15.0f, "%.2f"));
            sky.sunElevationDeg() = math::clamp(sky.sunElevationDeg(), -89.0f, 89.0f);
            sky.sunAzimuthDeg() = WrapDegrees360(sky.sunAzimuthDeg());
        }

        ImGui::Text("Presets:");
        if (ImGui::Button("Sunrise")) { sky.applySunPreset(5.0f, 85.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Morning")) { sky.applySunPreset(25.0f, 100.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Noon")) { sky.applySunPreset(65.0f, 180.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Golden hour")) { sky.applySunPreset(12.0f, 260.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Sunset")) { sky.applySunPreset(3.0f, 275.0f); changed = true; }

        ImGui::EndDisabled();

        const float3 dir = sky.computeSunDirection(sky.sunElevationDeg(), sky.sunAzimuthDeg());
        ImGui::Text("Direction: (%.3f, %.3f, %.3f)  elev=%.1f°  azim=%.1f°",
            dir.x, dir.y, dir.z, sky.sunElevationDeg(), sky.sunAzimuthDeg());
    }

    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        mark(ImGui::DragFloat("Sun illuminance", &sky.sunBrightness(), 0.01f, 0.0f, 64.0f));
        mark(ImGui::SliderFloat("Multi-scattering", &sky.multiScatteringFactor(), 0.0f, 2.0f));
        mark(ImGui::DragFloat("Sun angular diameter (deg)", &sky.sunAngularDiameterDeg(), 0.01f, 0.05f, 5.0f));
        mark(ImGui::DragFloat("Camera height (km)", &sky.cameraHeightKm(), 0.01f, 0.001f, 50.0f));
    }

    if (ImGui::CollapsingHeader("Atmosphere / Aerosols", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        ImGui::TextWrapped("Physical density and aerosol controls. Changes rebuild atmosphere LUTs.");

        mark(ImGui::DragFloat("Atmosphere height (km)", &sky.atmosphereHeightKm(), 0.1f, 20.0f, 300.0f));
        mark(ImGui::SliderFloat("Rayleigh scattering", &sky.rayleighScatteringScale(), 0.0f, 4.0f, "%.3f"));
        mark(ImGui::DragFloat("Rayleigh scale height (km)", &sky.rayleighHeightKm(), 0.05f, 1.0f, 30.0f));

        ImGui::SeparatorText("Mie aerosols");
        mark(ImGui::SliderFloat("Mie scattering", &sky.mieScatteringScale(), 0.0f, 10.0f, "%.3f"));
        mark(ImGui::SliderFloat("Mie absorption", &sky.mieAbsorptionScale(), 0.0f, 10.0f, "%.3f"));
        mark(ImGui::DragFloat("Mie scale height (km)", &sky.mieHeightKm(), 0.01f, 0.1f, 10.0f));
        mark(ImGui::SliderFloat("Mie anisotropy", &sky.mieAnisotropy(), 0.0f, 0.99f, "%.3f"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher values create a tighter forward-scattering halo around the sun.");

        ImGui::SeparatorText("Absorption / ground");
        mark(ImGui::SliderFloat("Ozone absorption", &sky.ozoneScale(), 0.0f, 4.0f, "%.3f"));
        mark(ImGui::ColorEdit3("Ground albedo", &sky.groundAlbedo().x, ImGuiColorEditFlags_Float));

        if (ImGui::Button("reset Earth atmosphere"))
        {
            sky.resetEarthAtmosphere();
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Aerial Perspective", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        mark(ImGui::Checkbox("Enable aerial perspective", &sky.aerialPerspectiveEnabled()));
        ImGui::BeginDisabled(!sky.aerialPerspectiveEnabled());
        mark(ImGui::DragFloat("World units to km", &sky.worldToKilometers(), 0.00001f, 0.000001f, 1.0f, "%.6f"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Default 0.001 assumes one world unit is one meter.");
        mark(ImGui::DragFloat("Aerial max distance (km)", &sky.aerialPerspectiveMaxDistanceKm(), 0.5f, 0.1f, 1000.0f));
        mark(ImGui::SliderInt("Aerial samples", &sky.aerialPerspectiveSampleCount(), 4, 32));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher values improve long-distance quality but increase full-screen cost.");
        ImGui::EndDisabled();
    }

    return changed;
}

bool DrawOpacityMicromapDebug(
    OpacityMicromapBuilder& builder,
    const caustica::scene::SceneRenderData& renderData,
    float indent)
{
    RAII_SCOPE(ImGui::PushID("OpacityMicromapBuilderDebugGUI");, ImGui::PopID(););

    bool resetAccumulation = false;
    OpacityMicroMapUIData& ui = builder.uiData();

    if (ImGui::Checkbox("Enable", &ui.Enable))
        resetAccumulation = true;

    {
        UI_SCOPED_DISABLE(ui.ActiveState.has_value() && ui.ActiveState->Format != caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State);
        if (ImGui::Checkbox("Force 2 State", &ui.Force2State))
            resetAccumulation = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Will force 2-State via TLAS instance mask.");
    }

    if (ImGui::Checkbox("render ONLY OMMs", &ui.OnlyOMMs))
        resetAccumulation = true;

    ImGui::Separator();
    ImGui::Text("Bake settings (Require Rebuild to take effect)");

    if (ui.BuildsLeftInQueue != 0)
    {
        const float progress = (1.f - (float)ui.BuildsLeftInQueue / ui.BuildsQueued);
        char label[64];
        snprintf(label, sizeof(label), "Build progress: %u%%", (uint32_t)(100.f * progress));
        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0), label);
    }

    {
        UI_SCOPED_DISABLE(ui.ActiveState.has_value() && ui.ActiveState == ui.DesiredState);
        if (ImGui::Button("Trigger Rebuild"))
            ui.TriggerRebuild = true;
    }

    ImGui::Checkbox("Dynamic subdivision level", &ui.DesiredState.EnableDynamicSubdivision);

    {
        UI_SCOPED_DISABLE(!ui.DesiredState.EnableDynamicSubdivision);
        ImGui::SliderFloat("Dynamic subdivision scale", &ui.DesiredState.DynamicSubdivisionScale, 0.01f, 20.f, "%.1f", ImGuiSliderFlags_Logarithmic);
    }

    {
        const int maxSubdivisionLevel = ui.DesiredState.ComputeOnly ? 12 : 10;
        ui.DesiredState.MaxSubdivisionLevel = std::clamp(ui.DesiredState.MaxSubdivisionLevel, 1, maxSubdivisionLevel);
        ImGui::SliderInt("Max subdivision level", &ui.DesiredState.MaxSubdivisionLevel, 1, maxSubdivisionLevel, "%d", ImGuiSliderFlags_AlwaysClamp);
    }

    {
        constexpr std::array<const char*, 3> formatNames = { "None", "Fast Trace", "Fast Build" };
        constexpr std::array<caustica::rhi::rt::OpacityMicromapBuildFlags, 3> formats = {
            caustica::rhi::rt::OpacityMicromapBuildFlags::None,
            caustica::rhi::rt::OpacityMicromapBuildFlags::FastTrace,
            caustica::rhi::rt::OpacityMicromapBuildFlags::FastBuild,
        };

        if (ImGui::BeginCombo("Flag", formatNames[(uint32_t)ui.DesiredState.Flag]))
        {
            for (uint i = 0; i < formats.size(); i++)
            {
                const bool selected = formats[i] == ui.DesiredState.Flag;
                if (ImGui::Selectable(formatNames[i], selected))
                    ui.DesiredState.Flag = formats[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    {
        auto formatToString = [](caustica::rhi::rt::OpacityMicromapFormat format) {
            return format == caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State ? "2-State" : "4-State";
        };
        const std::array<caustica::rhi::rt::OpacityMicromapFormat, 2> formats = {
            caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State,
            caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State,
        };
        if (ImGui::BeginCombo("Format", formatToString(ui.DesiredState.Format)))
        {
            for (uint i = 0; i < formats.size(); i++)
            {
                const bool selected = formats[i] == ui.DesiredState.Format;
                if (ImGui::Selectable(formatToString(formats[i]), selected))
                    ui.DesiredState.Format = formats[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    {
        auto stateToString = [](caustica::omm::OpacityState state) {
            const char* strings[] = { "Transparent", "Opaque", "UnknownTransparent", "UnknownOpaque" };
            const int index = (int)state;
            return (index >= 0 && index < 4) ? strings[index] : "Transparent";
        };
        const std::array<caustica::omm::OpacityState, 4> states = {
            caustica::omm::OpacityState::Transparent,
            caustica::omm::OpacityState::Opaque,
            caustica::omm::OpacityState::UnknownTransparent,
            caustica::omm::OpacityState::UnknownOpaque,
        };

        if (ImGui::BeginCombo("AlphaCutoffGT", stateToString(static_cast<caustica::omm::OpacityState>(ui.DesiredState.AlphaCutoffGT))))
        {
            for (uint i = 0; i < states.size(); i++)
            {
                const bool selected = states[i] == static_cast<caustica::omm::OpacityState>(ui.DesiredState.AlphaCutoffGT);
                if (ImGui::Selectable(stateToString(states[i]), selected))
                    ui.DesiredState.AlphaCutoffGT = (int)states[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::BeginCombo("AlphaCutoffLE", stateToString(static_cast<caustica::omm::OpacityState>(ui.DesiredState.AlphaCutoffLE))))
        {
            for (uint i = 0; i < states.size(); i++)
            {
                const bool selected = states[i] == static_cast<caustica::omm::OpacityState>(ui.DesiredState.AlphaCutoffLE);
                if (ImGui::Selectable(stateToString(states[i]), selected))
                    ui.DesiredState.AlphaCutoffLE = (int)states[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::CollapsingHeader("Debug settings"))
    {
        UI_SCOPED_INDENT(indent);

#if ENABLE_DEBUG_VIZUALISATIONS
        if (ImGui::Combo("Debug View", (int*)&ui.DebugView, "Disabled\0InWorld\0Overlay\0\0"))
            resetAccumulation = true;
#else
        ImGui::Text("Please enable ENABLE_DEBUG_VIZUALISATIONS for debug viz");
        ui.DebugView = OpacityMicroMapDebugView::Disabled;
#endif

        ImGui::Checkbox("Compute Only", &ui.DesiredState.ComputeOnly);
        ImGui::Checkbox("Enable \"Level Line Intersection\"", &ui.DesiredState.LevelLineIntersection);
        ImGui::Checkbox("Enable TexCoord deduplication", &ui.DesiredState.EnableTexCoordDeduplication);
        ImGui::Checkbox("Force 32-bit indices", &ui.DesiredState.Force32BitIndices);
        ImGui::Checkbox("Enable Special Indices", &ui.DesiredState.EnableSpecialIndices);
        ImGui::SliderInt("Max memory per OMM", &ui.DesiredState.MaxOmmArrayDataSizeInMB, 1, 1000, "%dMB", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Enable NSight debug mode", &ui.DesiredState.EnableNsightDebugMode);
    }

    ImGui::Separator();
    ImGui::Text("Stats");
    ImGui::Text("%u active OMMs", ui.BuildsQueued);

    if (ImGui::CollapsingHeader("Bake Stats"))
    {
        UI_SCOPED_INDENT(indent);
        std::vector<OmmBakeMeshStat> stats;
        builder.collectBakeStats(renderData, stats);
        for (const OmmBakeMeshStat& mesh : stats)
        {
            ImGui::Text("%s", mesh.debugName.c_str());
            UI_SCOPED_INDENT(indent);
            for (const OmmBakeGeometryStat& geom : mesh.geometries)
            {
                ImGui::Text("%.3f%% (%llu known, %llu unknown)",
                    geom.knownRatioPercent,
                    (unsigned long long)geom.known,
                    (unsigned long long)geom.unknown);
            }
        }
    }

    return resetAccumulation;
}

bool DrawZoomToolDebug(ZoomTool& zoom)
{
    ZoomTool::ZoomSettings& settings = zoom.settings();
    ImGui::Checkbox("enabled", &settings.enabled);
    ImGui::InputInt("ZoomFactor", &settings.ZoomFactor, 1);
    settings.ZoomFactor = caustica::math::clamp(settings.ZoomFactor, 2, 32);
    ImGui::InputInt2("BoxPos", &settings.BoxPos.x);
    ImGui::InputInt2("BoxSize", &settings.BoxSize.x);
    return false;
}

} // namespace caustica::editor
