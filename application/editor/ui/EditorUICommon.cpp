#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "common/EditorIcons.h"
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/Scene.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <game/GameScene.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

// Keep in sync with EditorTheme semantic tokens (TextWarning / TextCategory).
const ImVec4 warnColor = { 232.f / 255.f, 168.f / 255.f, 120.f / 255.f, 1.f };
const ImVec4 categoryColor = { 126.f / 255.f, 200.f / 255.f, 175.f / 255.f, 1.f };

namespace
{
    bool IsMeshInstanceEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        auto* comp = ew.world().tryGet<caustica::scene::MeshInstanceComponent>(entity);
        return comp != nullptr && comp->mesh != nullptr;
    }

    bool IsGaussianSplatEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        auto* comp = ew.world().tryGet<caustica::scene::GaussianSplatComponent>(entity);
        return comp != nullptr;
    }

    bool IsCameraEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        return ew.world().tryGet<caustica::scene::CameraComponent>(entity) != nullptr;
    }

    bool IsEnvironmentLightEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        return caustica::scene::tryGetEnvironmentLight(ew.world(), entity) != nullptr;
    }

    bool IsLocalLightEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        return caustica::scene::tryGetDirectionalLight(ew.world(), entity) != nullptr
            || caustica::scene::tryGetSpotLight(ew.world(), entity) != nullptr
            || caustica::scene::tryGetPointLight(ew.world(), entity) != nullptr;
    }

    bool IsLightEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        return IsEnvironmentLightEntity(ew, entity) || IsLocalLightEntity(ew, entity);
    }

    // Entities that appear as selectable leaves in Hierarchy (Blender Outliner style).
    bool IsHierarchyLeafEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        return IsMeshInstanceEntity(ew, entity)
            || IsGaussianSplatEntity(ew, entity)
            || IsCameraEntity(ew, entity)
            || IsLightEntity(ew, entity);
    }

    bool HasHierarchyEntity(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (entity == ecs::NullEntity) return false;
        if (IsHierarchyLeafEntity(ew, entity)) return true;
        for (ecs::Entity child : ew.getEntityChildren(entity))
            if (HasHierarchyEntity(ew, child)) return true;
        return false;
    }

    HierarchyTypeIcon ResolveHierarchyTypeIcon(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
    {
        if (IsMeshInstanceEntity(ew, entity))
            return HierarchyTypeIcon::Mesh;
        if (IsGaussianSplatEntity(ew, entity))
            return HierarchyTypeIcon::GaussianSplat;
        if (IsCameraEntity(ew, entity))
            return HierarchyTypeIcon::Camera;
        if (IsEnvironmentLightEntity(ew, entity))
            return HierarchyTypeIcon::EnvironmentLight;
        if (IsLocalLightEntity(ew, entity))
            return HierarchyTypeIcon::Light;
        return HierarchyTypeIcon::Group;
    }

    float WrapDegrees(float degrees)
    {
        degrees = std::fmod(degrees, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees;
    }
}

int ResolveGaussianSplatShadowMode(const EditorUIData& ui)
{
        if (!ui.render.settings.GaussianSplatShadows && ui.render.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = ui.render.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : ui.render.settings.GaussianSplatShadowsMode;
        return dm::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT);
    }

    bool GaussianSplatModeCombo(EditorUIData& ui)
    {
        int renderingMode = ResolveGaussianSplatShadowMode(ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED ? 1 : 0;
        if (!SettingsCombo(
                "Rendering Mode",
                &renderingMode,
                "Raster 3DGS (VS)\0Hybrid 3DGS + 3DGRT\0\0"))
            return false;

        if (renderingMode == 1)
        {
            ui.render.settings.GaussianSplatShadows = true;
            if (ui.render.settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
                ui.render.settings.GaussianSplatShadowsMode = GAUSSIAN_SPLAT_SHADOWS_HARD;
        }
        else
        {
            ui.render.settings.GaussianSplatShadows = false;
            ui.render.settings.GaussianSplatShadowsMode = GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        }
        ui.render.runtime.Invalidation.AccelerationStructRebuildRequested = true;
        ui.render.settings.ResetAccumulation = true;
        return true;
    }

    bool GaussianSplatShadowsModeCombo(EditorUIData& ui)
    {
        const bool wasEnabled = ResolveGaussianSplatShadowMode(ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        int shadowMode = ResolveGaussianSplatShadowMode(ui);

        ui.render.settings.GaussianSplatShadowsMode = shadowMode;
        ui.render.settings.GaussianSplatShadows = shadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        if (!SettingsCombo(
                "Shadows Mode",
                &shadowMode,
                "Shadows off\0Hard shadows\0Soft shadows\0\0"))
            return false;

        shadowMode = dm::clamp(shadowMode, GAUSSIAN_SPLAT_SHADOWS_DISABLED, GAUSSIAN_SPLAT_SHADOWS_SOFT);
        ui.render.settings.GaussianSplatShadowsMode = shadowMode;
        ui.render.settings.GaussianSplatShadows = shadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        if (wasEnabled != ui.render.settings.GaussianSplatShadows)
            ui.render.runtime.Invalidation.AccelerationStructRebuildRequested = true;
        ui.render.settings.ResetAccumulation = true;
        return true;
    }

    bool GaussianSplatSortingCombo(EditorUIData& ui)
    {
        const bool changed = SettingsCombo(
            "Sorting Method",
            &ui.render.settings.GaussianSplatSortingMode,
            "GPU sort\0Stochastic Splats\0\0");
        ui.render.settings.GaussianSplatSortingMode = dm::clamp(ui.render.settings.GaussianSplatSortingMode, 0, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("GPU sort uses the existing radix-sort path. Stochastic splats uses stable randomized order plus stochastic opacity accept/reject.");
        return changed;
    }

    bool GaussianSplatFormatCombo(const char* label, int* value)
    {
        const bool changed = SettingsCombo(label, value, "Float32\0Float16\0Uint8\0\0");
        *value = dm::clamp(*value, 0, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Storage format used by the 3DGS raster color/alpha and SH buffers in VRAM.");
        return changed;
    }

    bool GaussianSplatFTBCombo(EditorUIData& ui)
    {
        const bool changed = SettingsCombo(
            "FTB Sync Mode",
            &ui.render.settings.GaussianSplatFTBSyncMode,
            "Disabled (fast)\0Interlock\0\0");
        ui.render.settings.GaussianSplatFTBSyncMode = dm::clamp(ui.render.settings.GaussianSplatFTBSyncMode, 0, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Front-to-back depth synchronization mode. The current RTXPT overlay path does not write a 3DGS depth iso buffer yet.");
        return changed;
    }

    bool GaussianSplatRtxKernelDegreeCombo(EditorUIData& ui)
    {
        const bool changed = SettingsCombo("Kernel Degree", &ui.render.settings.GaussianSplatRtxKernelDegree,
            "0 (Linear)\0"
            "1 (Laplacian)\0"
            "2 (Quadratic)\0"
            "3 (Cubic)\0"
            "4 (Tesseractic)\0"
            "5 (Quintic)\0\0");
        ui.render.settings.GaussianSplatRtxKernelDegree = dm::clamp(ui.render.settings.GaussianSplatRtxKernelDegree, 0, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Kernel degree for the 3DGRT particle intersection shape. Changing it rebuilds Gaussian BLAS proxies.");
        return changed;
    }

    bool GaussianSplatRtxParticleFormatCombo(EditorUIData& ui)
    {
        int particleFormat = ui.render.settings.GaussianSplatUseAABBs ? 1 : 0;
        const bool changed = SettingsCombo(
            "Particle Format",
            &particleFormat,
            "Icosahedron\0AABB + parametric\0\0");
        if (changed)
        {
            ui.render.settings.GaussianSplatUseAABBs = particleFormat == 1;
            if (ui.render.settings.GaussianSplatUseAABBs)
                ui.render.settings.GaussianSplatUseTLASInstances = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shortcut for the 3DGS RTX acceleration proxy format. AABB + parametric forces TLAS instances.");
        return changed;
    }

namespace
{

bool NameMatchesFilter(const std::string& name, const char* filter)
{
    if (!filter || filter[0] == '\0')
        return true;
    if (name.empty())
        return false;
    std::string hay = name;
    std::string needle = filter;
    for (char& c : hay)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : needle)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hay.find(needle) != std::string::npos;
}

bool HierarchyNodePassesFilter(
    caustica::scene::SceneEntityWorld& ew,
    ecs::Entity entity,
    const char* filter)
{
    if (!filter || filter[0] == '\0')
        return HasHierarchyEntity(ew, entity);
    if (!HasHierarchyEntity(ew, entity))
        return false;

    std::string nodeName = ew.getEntityName(entity);
    if (NameMatchesFilter(nodeName, filter))
        return true;
    for (ecs::Entity child : ew.getEntityChildren(entity))
    {
        if (HierarchyNodePassesFilter(ew, child, filter))
            return true;
    }
    return false;
}

bool GetEntityEnabled(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity)
{
    if (auto* mesh = ew.world().tryGet<caustica::scene::MeshInstanceComponent>(entity))
        return mesh->enabled;
    if (auto* splat = ew.world().tryGet<caustica::scene::GaussianSplatComponent>(entity))
        return splat->splat.enabled;
    return true;
}

void SetEntityEnabled(caustica::scene::SceneEntityWorld& ew, ecs::Entity entity, bool enabled)
{
    if (auto* mesh = ew.world().tryGet<caustica::scene::MeshInstanceComponent>(entity))
        mesh->enabled = enabled;
    if (auto* splat = ew.world().tryGet<caustica::scene::GaussianSplatComponent>(entity))
        splat->splat.enabled = enabled;
}

} // namespace

void BuildHierarchyNodeUI(EditorUIData& ui, caustica::Scene& scene, ecs::Entity entity, const char* filter)
{
    auto* ew = scene.getEntityWorld();
    if (!ew || entity == ecs::NullEntity)
        return;
    if (!HierarchyNodePassesFilter(*ew, entity, filter))
        return;

    const bool isMeshEntity = IsMeshInstanceEntity(*ew, entity);
    const bool isGaussianSplatEntity = IsGaussianSplatEntity(*ew, entity);
    const bool isSelectable = IsHierarchyLeafEntity(*ew, entity);
    const bool showVisibilityToggle = isMeshEntity || isGaussianSplatEntity;
    const auto& children = ew->getEntityChildren(entity);

    bool hasVisibleChildren = false;
    for (ecs::Entity child : children)
    {
        if (HierarchyNodePassesFilter(*ew, child, filter))
        {
            hasVisibleChildren = true;
            break;
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
        | ImGuiTreeNodeFlags_AllowOverlap;
    if (!hasVisibleChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (ui.editor.SelectedEntity == entity)
        flags |= ImGuiTreeNodeFlags_Selected;

    if (hasVisibleChildren && !isSelectable)
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (filter && filter[0] != '\0' && hasVisibleChildren)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    std::string nodeName = ew->getEntityName(entity);
    if (nodeName.empty())
        nodeName = "<unnamed>";

    const bool enabled = GetEntityEnabled(*ew, entity);
    const ImU32 muted = ImGui::ColorConvertFloat4ToU32(GetEditorColors().TextMuted);
    const ImU32 textCol = enabled
        ? ImGui::ColorConvertFloat4ToU32(GetEditorColors().Text)
        : muted;

    const float iconSize = std::min(ImGui::GetFrameHeight() - 6.f, 14.f);
    constexpr float kIconTextGap = 3.f;
    // Pad the label so text starts after [type icon + gap], while the icon itself
    // sits at TreeNode label origin (to the right of the fold arrow — never over it).
    const float spaceW = std::max(1.f, ImGui::CalcTextSize(" ").x);
    const int padSpaces = std::max(1, static_cast<int>(std::ceil((iconSize + kIconTextGap) / spaceW)));
    const std::string paddedLabel = std::string(static_cast<size_t>(padSpaces), ' ') + nodeName;

    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(textCol));
    ImGui::SetNextItemAllowOverlap();
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", paddedLabel.c_str());
    const bool treeClicked = ImGui::IsItemClicked();
    ImGui::PopStyleColor();

    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float rowH = rowMax.y - rowMin.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float labelStartX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing();
    const float iconX = labelStartX;
    const HierarchyTypeIcon typeIcon = ResolveHierarchyTypeIcon(*ew, entity);
    DrawHierarchyTypeIcon(
        dl,
        ImVec2(iconX, rowMin.y + (rowH - iconSize) * 0.5f),
        ImVec2(iconX + iconSize, rowMin.y + (rowH - iconSize) * 0.5f + iconSize),
        typeIcon,
        enabled ? textCol : muted);

    bool eyeClicked = false;
    if (showVisibilityToggle)
    {
        const float eyeSize = rowH;
        const ImVec2 eyePos(rowMax.x - eyeSize - 2.f, rowMin.y);
        ImGui::SetCursorScreenPos(eyePos);
        eyeClicked = ImGui::InvisibleButton("##eye", ImVec2(eyeSize, eyeSize));
        const bool eyeHovered = ImGui::IsItemHovered();
        DrawIconGlyph(
            dl,
            eyePos,
            ImVec2(eyePos.x + eyeSize, eyePos.y + eyeSize),
            enabled ? Icon::Eye : Icon::Hide,
            eyeHovered ? textCol : muted);
        if (eyeHovered)
            ImGui::SetTooltip(enabled ? "Hide" : "Show");

        if (eyeClicked)
        {
            SetEntityEnabled(*ew, entity, !enabled);
            ui.render.settings.ResetAccumulation = true;
            if (isGaussianSplatEntity)
                ui.render.runtime.Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    if (isSelectable && treeClicked && !eyeClicked)
    {
        ui.editor.SelectedEntity = entity;
        ui.editor.SelectedGaussianSplat = isGaussianSplatEntity;
    }

    if (open && hasVisibleChildren)
    {
        for (ecs::Entity child : children)
            BuildHierarchyNodeUI(ui, scene, child, filter);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

dm::float3 QuaternionToEulerDegreesXYZ(const dm::dquat& rotation)
    {
        constexpr float rad2deg = 180.0f / 3.14159265f;
        const dm::double3x3 m = rotation.toMatrix();

        const double y = std::asin(dm::clamp(-m.m_data[2], -1.0, 1.0));
        const double cy = std::cos(y);

        double x = 0.0;
        double z = 0.0;
        if (std::abs(cy) > 1e-8)
        {
            x = std::atan2(m.m_data[5], m.m_data[8]);
            z = std::atan2(m.m_data[1], m.m_data[0]);
        }
        else
        {
            x = std::atan2(-m.m_data[7], m.m_data[4]);
        }

        return dm::float3(
            WrapDegrees(float(x) * rad2deg),
            WrapDegrees(float(y) * rad2deg),
            WrapDegrees(float(z) * rad2deg));
    }

    bool SameRotation(const dm::dquat& a, const dm::dquat& b)
    {
        const double lenA = dm::length(a);
        const double lenB = dm::length(b);
        if (lenA <= 1e-12 || lenB <= 1e-12)
            return false;

        const double cosine = std::abs(dm::dot(a / lenA, b / lenB));
        return cosine > 0.999999999;
    }

namespace
{

bool AxisDragFloat(char axis, ImU32 axisCol, float* value, float speed, float vMin, float vMax, const char* format, bool disabled)
{
    ImGui::PushID(static_cast<int>(axis));
    if (disabled)
        ImGui::BeginDisabled();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(36, 38, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(46, 50, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(52, 58, 68, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, ImGui::GetStyle().FramePadding.y));

    // Leading spaces reserve room for the axis letter drawn on top.
    char fmtBuf[32];
    std::snprintf(fmtBuf, sizeof(fmtBuf), "   %s", format);
    const bool changed = ImGui::DragFloat("##axis", value, speed, vMin, vMax, fmtBuf);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    const ImVec2 r0 = ImGui::GetItemRectMin();
    const ImVec2 r1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float strip = 3.f;
    dl->AddRectFilled(r0, ImVec2(r0.x + strip, r1.y), axisCol, 3.f, ImDrawFlags_RoundCornersLeft);
    const char letter[2] = { axis, '\0' };
    const ImVec2 ts = ImGui::CalcTextSize(letter);
    dl->AddText(
        ImVec2(r0.x + strip + 4.f, r0.y + (r1.y - r0.y - ts.y) * 0.5f),
        axisCol,
        letter);

    if (disabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

} // namespace

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
    bool lockUniform,
    TransformVec3RowEditInfo* editInfo)
{
    ImGui::PushID(id);

    bool changed = false;
    const float gap = 3.f;
    constexpr float kLabelColW = 70.f;

    auto noteItemEdit = [&]()
    {
        if (!editInfo)
            return;
        if (ImGui::IsItemActivated())
            editInfo->activated = true;
        if (ImGui::IsItemDeactivated())
            editInfo->deactivated = true;
        if (ImGui::IsItemDeactivatedAfterEdit())
            editInfo->deactivatedAfterEdit = true;
    };

    if (IconButton("##reset", Icon::Refresh, false, "Reset"))
    {
        values[0] = resetValues[0];
        values[1] = resetValues[1];
        values[2] = resetValues[2];
        changed = true;
    }
    noteItemEdit();
    // Reset is a discrete click — treat as a complete edit even if ImGui does not
    // report DeactivatedAfterEdit for the icon button widget.
    if (changed && editInfo)
    {
        editInfo->activated = true;
        editInfo->deactivated = true;
        editInfo->deactivatedAfterEdit = true;
    }
    ImGui::SameLine(0.f, gap);

    const bool isLocked = locked && *locked;
    if (IconButton(
            "##lock",
            isLocked ? Icon::Lock : Icon::Unlock,
            isLocked,
            lockUniform ? "Uniform scale" : "Lock values"))
    {
        if (locked)
            *locked = !*locked;
    }
    ImGui::SameLine(0.f, gap);

    const float labelX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(labelX + kLabelColW);

    const float avail = ImGui::GetContentRegionAvail().x;
    const float fieldW = std::max(48.f, (avail - gap * 2.f) / 3.f);

    const ImU32 axisCols[3] = {
        IM_COL32(220, 72, 72, 255),
        IM_COL32(88, 180, 96, 255),
        IM_COL32(72, 140, 230, 255),
    };
    const char axes[3] = { 'X', 'Y', 'Z' };

    const bool disableEdits = locked && *locked && !lockUniform;
    const bool uniform = lockUniform && locked && *locked;

    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
            ImGui::SameLine(0.f, gap);
        ImGui::SetNextItemWidth(fieldW);
        if (AxisDragFloat(axes[i], axisCols[i], &values[i], speed, vMin, vMax, format, disableEdits))
        {
            changed = true;
            if (uniform)
                values[0] = values[1] = values[2] = values[i];
        }
        noteItemEdit();
    }

    ImGui::PopID();
    return changed;
}

namespace
{

void InspectorBeginLabeledRow(const char* label)
{
    const float rowStartX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(rowStartX + kInspectorLabelWidth);
    ImGui::SetNextItemWidth(std::max(40.f, ImGui::GetContentRegionAvail().x));
}

} // namespace

bool InspectorDragFloat(const char* label, float* v, float speed, float vMin, float vMax, const char* format)
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool changed = ImGui::DragFloat("##v", v, speed, vMin, vMax, format);
    ImGui::PopID();
    return changed;
}

bool InspectorDragFloat3(const char* label, float v[3], float speed, float vMin, float vMax, const char* format)
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool changed = ImGui::DragFloat3("##v", v, speed, vMin, vMax, format);
    ImGui::PopID();
    return changed;
}

bool InspectorDragInt(const char* label, int* v, float speed, int vMin, int vMax)
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool changed = ImGui::DragInt("##v", v, speed, vMin, vMax);
    ImGui::PopID();
    return changed;
}

bool InspectorCheckbox(const char* label, bool* v)
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool changed = ImGui::Checkbox("##v", v);
    ImGui::PopID();
    return changed;
}

bool InspectorColorEdit3(const char* label, float color[3])
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool changed = ImGui::ColorEdit3(
        "##v",
        color,
        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
            | ImGuiColorEditFlags_DisplayRGB);
    ImGui::PopID();
    return changed;
}

bool InspectorBeginCombo(const char* label, const char* preview)
{
    ImGui::PushID(label);
    InspectorBeginLabeledRow(label);
    const bool open = ImGui::BeginCombo("##v", preview);
    if (!open)
        ImGui::PopID();
    return open; // when true, caller must EndCombo() then PopID()
}

namespace
{

void SettingsBeginLabeledRow(const char* label)
{
    const float rowStartX = ImGui::GetCursorPosX();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopStyleColor();

    // On narrow dock layouts, stack the value below its label instead of
    // forcing two columns to overlap or spill outside the window.
    if (availableWidth < 190.f)
    {
        ImGui::SetNextItemWidth(std::max(56.f, availableWidth));
        return;
    }

    const float labelWidth =
        std::clamp(availableWidth * 0.42f, 96.f, kRenderSettingsLabelWidth);
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(rowStartX + labelWidth);
    ImGui::SetNextItemWidth(std::max(56.f, ImGui::GetContentRegionAvail().x));
}

} // namespace

bool SettingsCheckbox(const char* label, bool* v)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed = ImGui::Checkbox("##value", v);
    ImGui::PopID();
    return changed;
}

bool SettingsInputFloat(
    const char* label,
    float* v,
    float step,
    float stepFast,
    const char* format,
    ImGuiInputTextFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed = ImGui::InputFloat("##value", v, step, stepFast, format, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsInputInt(
    const char* label,
    int* v,
    int step,
    int stepFast,
    ImGuiInputTextFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed = ImGui::InputInt("##value", v, step, stepFast, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsDragFloat(
    const char* label,
    float* v,
    float speed,
    float vMin,
    float vMax,
    const char* format,
    ImGuiSliderFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed =
        ImGui::DragFloat("##value", v, speed, vMin, vMax, format, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsDragInt(
    const char* label,
    int* v,
    float speed,
    int vMin,
    int vMax,
    const char* format,
    ImGuiSliderFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed =
        ImGui::DragInt("##value", v, speed, vMin, vMax, format, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsSliderFloat(
    const char* label,
    float* v,
    float vMin,
    float vMax,
    const char* format,
    ImGuiSliderFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed =
        ImGui::SliderFloat("##value", v, vMin, vMax, format, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsSliderInt(
    const char* label,
    int* v,
    int vMin,
    int vMax,
    const char* format,
    ImGuiSliderFlags flags)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed =
        ImGui::SliderInt("##value", v, vMin, vMax, format, flags);
    ImGui::PopID();
    return changed;
}

bool SettingsCombo(const char* label, int* currentItem, const char* items)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool changed = ImGui::Combo("##value", currentItem, items);
    ImGui::PopID();
    return changed;
}

bool SettingsBeginCombo(const char* label, const char* preview)
{
    ImGui::PushID(label);
    SettingsBeginLabeledRow(label);
    const bool open = ImGui::BeginCombo("##value", preview);
    if (!open)
        ImGui::PopID();
    return open;
}

void SettingsEndCombo()
{
    ImGui::EndCombo();
    ImGui::PopID();
}

void SettingsCategoryHeader(const char* label)
{
    ImGui::Spacing();
    ImGui::TextColored(categoryColor, "%s", label);
    ImGui::Separator();
    ImGui::Spacing();
}

const ::PerformancePreset s_performancePresets[kPerformancePresetCount] = {
    //                                    NEECand  NEEFull  NEEMIS  SPP  Bounce  DiffBnc   TexLOD  NestDiel  EnvMIP  SPActive  PrimRepl  Bloom    LDSampl    FflyTrhld    DLSS (on separate line due to macros)
    { "Ultra Performance",                3,       1,       1,      1,   10,     1,        0.0f,   1,        3,      2,        false,    false,   false,     0.01,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eUltraPerformance,
#endif
    },
    { "Performance",                      3,       1,       1,      1,   12,     1,       -0.5f,   1,        2,      3,        true,     true,    false,     0.05,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eMaxPerformance,
#endif
    },
    { "Balanced",                         5,       1,       1,      1,   18,     2,       -1.0f,   1,        2,      3,        true,     true,    true,      0.1,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eBalanced,
#endif
    },
    { "Quality",                          3,       2,       1,      1,   24,     3,       -1.5f,   1,        2,      3,        true,     true,    true,      0.2,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eMaxQuality,
#endif
    },
    { "Ultra Quality",                    3,       2,       0,      1,   48,     3,       -1.5f,   2,        1,      3,        true,     true,    true,      1.0,
#if CAUSTICA_WITH_ANY_DLSS
        SI::DLSSMode::eDLAA,
#endif
    },
};
bool MatchesPreset(const EditorUIData& ui, const ::PerformancePreset& p)
{
    return caustica::render::MatchesPerformancePreset(ui.render.settings, p);
}

void ApplyPreset(EditorUIData& ui, const ::PerformancePreset& p)
{
    caustica::render::applyPerformancePreset(ui.render.settings, p);
}

} // namespace caustica::editor

namespace caustica::editor
{

#if CAUSTICA_WITH_ANY_DLSS
SI::DLSSMode DLSSModeUI(SI::DLSSMode dlssModeCurrent)
{
    int current = -1;
    switch (dlssModeCurrent)
    {
    case SI::DLSSMode::eMaxPerformance:    current = 1; break;
    case SI::DLSSMode::eBalanced:          current = 2; break;
    case SI::DLSSMode::eMaxQuality:        current = 3; break;
    case SI::DLSSMode::eUltraPerformance:  current = 0; break;
    case SI::DLSSMode::eDLAA:              current = 4; break;
    default: assert(false); return SI::DLSSMode::eBalanced;
    }

    ImGui::Combo("DLSS Resolution Scale", (int*)&current, "UltraPerformance\0Performance\0Balanced\0Quality\0DLAA\0");

    switch (current)
    {
    case 0 : return SI::DLSSMode::eUltraPerformance;
    case 1 : return SI::DLSSMode::eMaxPerformance;
    case 2 : return SI::DLSSMode::eBalanced;
    case 3 : return SI::DLSSMode::eMaxQuality;
    case 4 : return SI::DLSSMode::eDLAA;
    default: assert(false); return SI::DLSSMode::eBalanced;

    }
    ImGui::Text("(DLSS setting also apply to Ray Reconstruction)");
}
#endif

std::string TrimTogglable(const std::string text)
{
    size_t tog = text.rfind("_togglable");
    if (tog != std::string::npos)
        return text.substr(0, tog);
    return text;
}
std::string TrimSkyDisplayName(std::string text)
{
    if (text == c_EnvMapSceneDefault)
        return "default";
    else if (text == c_EnvMapProcSky)
        return "sky (manual)";
    else if (text == c_EnvMapProcSky_Morning)
        return "morning";
    else if (text == c_EnvMapProcSky_Midday)
        return "midday";
    else if (text == c_EnvMapProcSky_Evening)
        return "evening";
    else if (text == c_EnvMapProcSky_Dawn)
        return "dawn";
    else if (text == c_EnvMapProcSky_PitchBlack)
        return "pitch black";
    return "unknown";
}

bool TogglableNode::IsSelected() const
{
    if (!EntityWorld || Entity == ecs::NullEntity) return false;
    auto* comp = EntityWorld->world().tryGet<caustica::scene::LocalTransformComponent>(Entity);
    if (!comp) return false;
    return all(comp->translation == OriginalTranslation);
}

void TogglableNode::SetSelected(bool selected)
{
    if (!EntityWorld || Entity == ecs::NullEntity) return;
    if (selected)
        EntityWorld->setTranslation(Entity, OriginalTranslation);
    else
        EntityWorld->setTranslation(Entity, {-10000.0, -10000.0, -10000.0});
}

void UpdateTogglableNodes(std::vector<TogglableNode>& togglableNodes, caustica::scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    if (entity == ecs::NullEntity) return;

    auto addIfTogglable = [&](const std::string& token, ecs::Entity e) -> TogglableNode*
    {
        const size_t tokenLen = token.length();
        const std::string name = entityWorld.getEntityName(e);
        const size_t nameLen = name.length();
        if (nameLen > tokenLen && name.substr(nameLen - tokenLen) == token)
        {
            TogglableNode tn;
            tn.Entity = e;
            tn.EntityWorld = &entityWorld;
            tn.UIName = name.substr(0, nameLen - tokenLen);
            auto* comp = entityWorld.world().tryGet<caustica::scene::LocalTransformComponent>(e);
            tn.OriginalTranslation = comp ? comp->translation : dm::double3(0.0);
            togglableNodes.push_back(tn);
            return &togglableNodes.back();
        }
        return nullptr;
    };

    TogglableNode* justAdded = addIfTogglable("_togglable", entity);
    if (justAdded == nullptr)
    {
        justAdded = addIfTogglable("_togglable_off", entity);
        if (justAdded != nullptr)
            justAdded->SetSelected(false);
    }

    const auto& children = entityWorld.getEntityChildren(entity);
    for (int i = (int)children.size() - 1; i >= 0; i--)
        UpdateTogglableNodes(togglableNodes, entityWorld, children[i]);
}

} // namespace caustica::editor
