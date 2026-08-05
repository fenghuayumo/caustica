#include "ui/EditorUIInternal.h"

#include <algorithm>

namespace caustica::editor
{

namespace
{

void ResetButtonRow(PathTracerSettings& settings)
{
    if (!ImGui::BeginTable(
            "##RenderResetActions",
            2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings
                | ImGuiTableFlags_NoPadOuterX))
        return;

    ImGui::TableNextColumn();
    if (ImGui::Button("Reset Accumulation", ImVec2(-1.f, 0.f)))
        settings.ResetAccumulation = true;
    ImGui::TableNextColumn();
    if (ImGui::Button("Reset Caches", ImVec2(-1.f, 0.f)))
        settings.ResetRealtimeCaches = true;
    ImGui::EndTable();
}

} // namespace

void EditorUI::BuildRenderSettingsOverview(const PanelLayout& layout)
{
    RAII_SCOPE(ImGui::PushID("RenderSettingsOverview");, ImGui::PopID(););

    if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

        int mode = m_settings.RealtimeMode ? 1 : 0;
        if (SettingsCombo("Mode", &mode, "Reference\0Realtime\0\0"))
        {
            m_settings.RealtimeMode = mode == 1;
            m_settings.ResetAccumulation = true;
            m_settings.ResetRealtimeCaches = true;
        }

        const char* currentPreset = "Custom";
        int currentPresetIndex = -1;
        for (int i = 0; i < static_cast<int>(kPerformancePresetCount); ++i)
        {
            if (MatchesPreset(m_ui, s_performancePresets[i]))
            {
                currentPreset = s_performancePresets[i].Name;
                currentPresetIndex = i;
                break;
            }
        }
        if (SettingsBeginCombo("Quality", currentPreset))
        {
            for (int i = 0; i < static_cast<int>(kPerformancePresetCount); ++i)
            {
                const bool selected = currentPresetIndex == i;
                if (ImGui::Selectable(s_performancePresets[i].Name, selected))
                {
                    ApplyPreset(m_ui, s_performancePresets[i]);
                    m_settings.ResetRealtimeCaches = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            SettingsEndCombo();
        }

        if (m_settings.RealtimeMode)
        {
            const bool restirActive =
                m_settings.actualUseReSTIRDI() || m_settings.actualUseReSTIRGI()
                || m_settings.actualUseReSTIRPT();
            UI_SCOPED_DISABLE(restirActive);
            if (SettingsInputInt("Samples / Pixel", &m_settings.RealtimeSamplesPerPixel))
            {
                m_settings.RealtimeSamplesPerPixel =
                    dm::clamp(m_settings.RealtimeSamplesPerPixel, 1, 64);
                m_settings.ResetAccumulation = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && restirActive)
                ImGui::SetTooltip("ReSTIR modes trace one primary path per pixel.");
        }
        else
        {
            if (SettingsInputInt("Target Samples", &m_settings.AccumulationTarget))
            {
                m_settings.AccumulationTarget =
                    dm::clamp(m_settings.AccumulationTarget, 1, 4 * 1024 * 1024);
                m_settings.ResetAccumulation = true;
            }
        }

        if (SettingsInputInt("Max Bounces", &m_settings.BounceCount))
        {
            m_settings.BounceCount = dm::clamp(m_settings.BounceCount, 0, MAX_BOUNCE_COUNT);
            m_settings.ResetAccumulation = true;
        }

        ResetButtonRow(m_settings);
    }

    if (ImGui::CollapsingHeader("AA & Denoising", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

        if (m_settings.RealtimeMode)
        {
#if CAUSTICA_WITH_ANY_DLSS
            const bool dlssAvailable = m_settings.IsDLSSSuported;
            const bool dlssRRAvailable = m_settings.IsDLSSRRSupported;
#else
            const bool dlssAvailable = false;
            const bool dlssRRAvailable = false;
#endif
            const char* labels[] = { "Disabled", "TAA", "DLSS", "DLSS-RR" };
            m_settings.RealtimeAA = dm::clamp(m_settings.RealtimeAA, 0, 3);
            if (SettingsBeginCombo("Method", labels[m_settings.RealtimeAA]))
            {
                for (int i = 0; i < IM_ARRAYSIZE(labels); ++i)
                {
                    const bool available =
                        i < 2 || (i == 2 && dlssAvailable) || (i == 3 && dlssRRAvailable);
                    UI_SCOPED_DISABLE(!available);
                    const bool selected = m_settings.RealtimeAA == i;
                    if (ImGui::Selectable(labels[i], selected) && available)
                    {
                        m_settings.RealtimeAA = i;
                        m_settings.ResetAccumulation = true;
                        m_settings.ResetRealtimeCaches = true;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                SettingsEndCombo();
            }

#if CAUSTICA_WITH_ANY_DLSS
            if (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3)
                m_settings.DLSSMode = DLSSModeUI(m_settings.DLSSMode);
#endif

            UI_SCOPED_DISABLE(m_settings.RealtimeAA == 3);
            if (SettingsCheckbox("Standalone NRD", &m_settings.StandaloneDenoiser))
            {
                m_settings.ResetAccumulation = true;
                m_settings.ResetRealtimeCaches = true;
            }
        }
        else
        {
#if CAUSTICA_WITH_OIDN
            if (SettingsCheckbox("OIDN", &m_settings.ReferenceOIDNDenoiser))
                m_settings.ReferenceOIDNDenoiserChanged = true;
            UI_SCOPED_DISABLE(!m_settings.ReferenceOIDNDenoiser);
            if (SettingsCheckbox("Use GPU", &m_settings.ReferenceOIDNUseGPU))
                m_settings.ReferenceOIDNDenoiserChanged = true;
#else
            bool unavailable = false;
            UI_SCOPED_DISABLE(true);
            SettingsCheckbox("OIDN", &unavailable);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("OIDN support is disabled in this build.");
#endif
        }

        RESET_ON_CHANGE(SettingsCheckbox("Tone Mapping", &m_settings.EnableToneMapping));
    }

    if (m_runtime.GaussianSplats.SplatCount > 0
        && ImGui::CollapsingHeader("Gaussian Splats", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
        RESET_ON_CHANGE(GaussianSplatPrimaryMethodCombo(m_ui));
        RESET_ON_CHANGE(GaussianSplatShadowsModeCombo(m_ui));
        RESET_ON_CHANGE(SettingsCheckbox(
            "Mip Antialiasing", &m_settings.GaussianSplatMipAntialiasing));
    }
}

void EditorUI::BuildAdvancedRenderSettings(const PanelLayout& layout)
{
    RAII_SCOPE(ImGui::PushID("AdvancedRenderSettings");, ImGui::PopID(););

    if (ImGui::CollapsingHeader("Advanced Path Tracing"))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

        RESET_ON_CHANGE(SettingsInputInt(
            "Diffuse Bounces", &m_settings.DiffuseBounceCount));
        m_settings.DiffuseBounceCount =
            dm::clamp(m_settings.DiffuseBounceCount, 0, MAX_BOUNCE_COUNT);

        bool& fireflyEnabled = m_settings.RealtimeMode
            ? m_settings.RealtimeFireflyFilterEnabled
            : m_settings.ReferenceFireflyFilterEnabled;
        float& fireflyThreshold = m_settings.RealtimeMode
            ? m_settings.RealtimeFireflyFilterThreshold
            : m_settings.ReferenceFireflyFilterThreshold;
        RESET_ON_CHANGE(SettingsCheckbox("Firefly Filter", &fireflyEnabled));
        if (fireflyEnabled)
            RESET_ON_CHANGE(SettingsInputFloat(
                "Firefly Threshold", &fireflyThreshold, 0.01f, 0.1f, "%.5f"));

        RESET_ON_CHANGE(SettingsInputFloat("Texture MIP Bias", &m_settings.TexLODBias));
        RESET_ON_CHANGE(SettingsCombo(
            "Nested Dielectrics",
            &m_settings.NestedDielectricsQuality,
            "Off\0Fast\0Quality\0\0"));
        m_settings.NestedDielectricsQuality =
            dm::clamp(m_settings.NestedDielectricsQuality, 0, 2);
    }

    if (ImGui::CollapsingHeader("Advanced Lighting"))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););

        RESET_ON_CHANGE(SettingsCheckbox("Next Event Estimation", &m_settings.UseNEE));
        if (m_settings.UseNEE)
        {
            RESET_ON_CHANGE(SettingsCombo(
                "NEE Method", &m_settings.NEEType, "Uniform\0Power\0Adaptive\0\0"));
            RESET_ON_CHANGE(SettingsInputInt(
                "Candidate Samples", &m_settings.NEECandidateSamples));
            RESET_ON_CHANGE(SettingsInputInt("Full Samples", &m_settings.NEEFullSamples));
            m_settings.NEECandidateSamples = dm::clamp(
                m_settings.NEECandidateSamples, 1, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
            m_settings.NEEFullSamples = dm::clamp(
                m_settings.NEEFullSamples, 0, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
        }

        if (m_settings.RealtimeMode)
        {
            if (SettingsCheckbox("ReSTIR DI", &m_settings.UseReSTIRDI))
            {
                m_settings.ResetAccumulation = true;
                m_settings.ResetRealtimeCaches = true;
            }
            if (SettingsCheckbox("ReSTIR GI", &m_settings.UseReSTIRGI)
                && m_settings.UseReSTIRGI)
            {
                m_settings.UseReSTIRPT = false;
                m_settings.ResetAccumulation = true;
                m_settings.ResetRealtimeCaches = true;
            }
            if (SettingsCheckbox("ReSTIR PT", &m_settings.UseReSTIRPT)
                && m_settings.UseReSTIRPT)
            {
                m_settings.UseReSTIRGI = false;
                m_settings.ResetAccumulation = true;
                m_settings.ResetRealtimeCaches = true;
            }

            int restirPreset = static_cast<int>(m_settings.RTXDIRestirPreset);
            if (SettingsCombo(
                    "DI / GI Preset",
                    &restirPreset,
                    "Custom\0Fast\0Medium\0Unbiased\0Ultra\0Reference\0\0"))
            {
                m_settings.RTXDIRestirPreset =
                    static_cast<RTXDIRestirQualityPreset>(restirPreset);
                m_settings.applyRTXDIRestirPreset();
                m_settings.ResetRealtimeCaches = true;
            }
            int restirPtPreset = static_cast<int>(m_settings.RTXDIRestirPTPreset);
            if (SettingsCombo(
                    "PT Preset",
                    &restirPtPreset,
                    "Custom\0Fast\0Medium\0Ultra\0\0"))
            {
                m_settings.RTXDIRestirPTPreset =
                    static_cast<RTXDIRestirPTQualityPreset>(restirPtPreset);
                m_settings.applyRTXDIRestirPTPreset();
                m_settings.ResetRealtimeCaches = true;
            }
        }
    }

    if (m_runtime.GaussianSplats.SplatCount > 0
        && ImGui::CollapsingHeader("Advanced Gaussian Splats"))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent););
        RESET_ON_CHANGE(GaussianSplatSortingCombo(m_ui));
        RESET_ON_CHANGE(GaussianSplatFormatCombo(
            "SH Format", &m_settings.GaussianSplatSHFormat));
        RESET_ON_CHANGE(SettingsCheckbox(
            "Quantize Normals", &m_settings.GaussianSplatQuantizeNormals));
        RESET_ON_CHANGE(SettingsCombo(
            "Frustum Culling",
            &m_settings.GaussianSplatFrustumCulling,
            "Disabled\0Distance Stage\0Raster Stage\0\0"));
        RESET_ON_CHANGE(SettingsCheckbox(
            "Screen Size Culling", &m_settings.GaussianSplatScreenSizeCulling));
        if (m_settings.GaussianSplatScreenSizeCulling)
            RESET_ON_CHANGE(SettingsDragFloat(
                "Min Pixel Coverage",
                &m_settings.GaussianSplatMinPixelCoverage,
                0.1f,
                0.1f,
                20.f,
                "%.2f"));
    }

    ImGui::Spacing();
    ImGui::TextDisabled("More tuning: press ` for Command Bar, then `cvar.list r[.].*`");
}

} // namespace caustica::editor
