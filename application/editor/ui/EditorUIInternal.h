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
void BuildHierarchyNodeUI(EditorUIData& ui, caustica::Scene& scene, ecs::Entity entity);
dm::float3 QuaternionToEulerDegreesXYZ(const dm::dquat& rotation);
bool SameRotation(const dm::dquat& a, const dm::dquat& b);

#if CAUSTICA_WITH_ANY_DLSS
SI::DLSSMode DLSSModeUI(SI::DLSSMode dlssModeCurrent);
#endif

} // namespace caustica::editor
