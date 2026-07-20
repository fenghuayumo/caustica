#pragma once

#include "ui/ui_macros.h"
#include "EditorUI.h"
#include "EditorUIData.h"
#include "common/EditorTheme.h"

#include <render/core/PathTracerSettings.h>
#include <imgui.h>
#include <math/math.h>
#include <ecs/Entity.h>
#include <scene/SceneEcs.h>
#include <string>
#include <cstddef>

namespace caustica::editor
{

#define RESET_ON_CHANGE(code) do{if (code) m_settings.ResetAccumulation = true;} while(false)

extern const ImVec4 warnColor;
extern const ImVec4 categoryColor;

inline constexpr std::size_t kPerformancePresetCount = 5;
extern const ::PerformancePreset s_performancePresets[kPerformancePresetCount];
bool MatchesPreset(const EditorUIData& ui, const ::PerformancePreset& p);
void ApplyPreset(EditorUIData& ui, const ::PerformancePreset& p);

std::string TrimTogglable(const std::string text);
std::string TrimSkyDisplayName(std::string text);

int ResolveGaussianSplatShadowMode(const EditorUIData& ui);
bool GaussianSplatModeCombo(EditorUIData& ui);
bool GaussianSplatShadowsModeCombo(EditorUIData& ui);
bool GaussianSplatSortingCombo(EditorUIData& ui);
bool GaussianSplatFTBCombo(EditorUIData& ui);
bool GaussianSplatRtxKernelDegreeCombo(EditorUIData& ui);
bool GaussianSplatRtxParticleFormatCombo(EditorUIData& ui);
void BuildHierarchyNodeUI(EditorUIData& ui, caustica::Scene& scene, ecs::Entity entity, const char* filter);
dm::float3 QuaternionToEulerDegreesXYZ(const dm::dquat& rotation);
bool SameRotation(const dm::dquat& a, const dm::dquat& b);

// Colored XYZ transform row (reset / lock / label / axis fields), matching DCC-style inspectors.
// When lockUniform is non-null and true, editing any axis copies that value to all three (Scale).
// When lockUniform is null, locked disables editing.
// editInfo (optional): activated / deactivatedAfterEdit aggregated across reset + XYZ fields.
struct TransformVec3RowEditInfo
{
    bool activated = false;
    bool deactivated = false;
    bool deactivatedAfterEdit = false;
};

bool TransformVec3Row(
    const char* id,
    const char* label,
    float values[3],
    float speed,
    float vMin,
    float vMax,
    const char* format,
    const float resetValues[3],
    bool* locked,
    bool lockUniform = false,
    TransformVec3RowEditInfo* editInfo = nullptr);

// Inspector property rows: fixed label column on the left, control fills the right.
inline constexpr float kInspectorLabelWidth = 148.f;
bool InspectorDragFloat(const char* label, float* v, float speed, float vMin, float vMax, const char* format);
bool InspectorDragFloat3(const char* label, float v[3], float speed, float vMin, float vMax, const char* format);
bool InspectorDragInt(const char* label, int* v, float speed, int vMin, int vMax);
bool InspectorCheckbox(const char* label, bool* v);
bool InspectorColorEdit3(const char* label, float color[3]);
bool InspectorBeginCombo(const char* label, const char* preview);

// Render Settings property rows: muted aligned label on the left, value control
// stretching on the right; narrow docks automatically stack label over value.
inline constexpr float kRenderSettingsLabelWidth = 126.f;
bool SettingsCheckbox(const char* label, bool* v);
bool SettingsInputFloat(
    const char* label,
    float* v,
    float step = 0.f,
    float stepFast = 0.f,
    const char* format = "%.3f",
    ImGuiInputTextFlags flags = 0);
bool SettingsInputInt(
    const char* label,
    int* v,
    int step = 1,
    int stepFast = 100,
    ImGuiInputTextFlags flags = 0);
bool SettingsDragFloat(
    const char* label,
    float* v,
    float speed,
    float vMin,
    float vMax,
    const char* format = "%.3f",
    ImGuiSliderFlags flags = 0);
bool SettingsDragInt(
    const char* label,
    int* v,
    float speed,
    int vMin,
    int vMax,
    const char* format = "%d",
    ImGuiSliderFlags flags = 0);
bool SettingsSliderFloat(
    const char* label,
    float* v,
    float vMin,
    float vMax,
    const char* format = "%.3f",
    ImGuiSliderFlags flags = 0);
bool SettingsSliderInt(
    const char* label,
    int* v,
    int vMin,
    int vMax,
    const char* format = "%d",
    ImGuiSliderFlags flags = 0);
bool SettingsCombo(const char* label, int* currentItem, const char* items);
bool SettingsBeginCombo(const char* label, const char* preview);
void SettingsEndCombo();
void SettingsCategoryHeader(const char* label);

#if CAUSTICA_WITH_ANY_DLSS
SI::DLSSMode DLSSModeUI(SI::DLSSMode dlssModeCurrent);
#endif

} // namespace caustica::editor
