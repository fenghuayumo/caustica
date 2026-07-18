#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include <engine/SceneQuery.h>
#include "common/ImGuiManager.h"
#include "common/TransformGizmo.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
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

void EditorUI::BuildInspectorPanel(const PanelLayout& layout)
{
    // Keep the dock slot alive even with no selection.
    if (!m_editorUI.ShowInspector)
        return;

    auto* ew = caustica::entityWorld(*m_sceneEditor.app());
    ImGui::Begin("Inspector", &m_editorUI.ShowInspector);
    (void)layout;

    if (!ew || m_editorUI.SelectedEntity == ecs::NullEntity)
    {
        ImGui::TextDisabled("No selection");
        ImGui::End();
        return;
    }

    const ecs::Entity entity = m_editorUI.SelectedEntity;
    std::string entityName = ew->getEntityName(entity);
    if (entityName.empty())
        entityName = "<unnamed>";

    auto* meshComp = ew->world().tryGet<caustica::scene::MeshInstanceComponent>(entity);
    auto* splatComp = ew->world().tryGet<caustica::scene::GaussianSplatComponent>(entity);
    caustica::GaussianSplat* gaussianSplat = splatComp ? &splatComp->splat : nullptr;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));

    ImGui::TextUnformatted(entityName.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    if (meshComp && meshComp->mesh)
        ImGui::Text("Mesh · %s", meshComp->mesh->name.c_str());
    else if (gaussianSplat)
        ImGui::Text("3DGS · %u splats", gaussianSplat->loadedSplatCount);
    else
        ImGui::TextUnformatted("Group");
    ImGui::PopStyleColor();

    if (meshComp || gaussianSplat)
    {
        bool visible = meshComp ? meshComp->enabled : gaussianSplat->enabled;
        if (InspectorCheckbox("Visible", &visible))
        {
            if (meshComp)
                meshComp->enabled = visible;
            if (gaussianSplat)
            {
                gaussianSplat->enabled = visible;
                m_runtime.Invalidation.AccelerationStructRebuildRequested = true;
            }
            m_settings.ResetAccumulation = true;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Section header with accent marker (DCC-style Transform block).
    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const float hh = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(hp.x + 6.f, hp.y + hh * 0.5f),
            4.5f,
            ImGui::ColorConvertFloat4ToU32(GetEditorColors().Accent));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.f);
    }
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(entity);
        dm::double3 translation = ltc ? ltc->translation : dm::double3(0.0);
        dm::dquat rotation = ltc ? ltc->rotation : dm::dquat::identity();
        dm::double3 scaling = ltc ? ltc->scaling : dm::double3(1.0);

        static bool lockPos = false;
        static bool lockRot = false;
        static bool lockScl = false;

        float pos[3] = { float(translation.x), float(translation.y), float(translation.z) };
        constexpr float kResetPos[3] = { 0.f, 0.f, 0.f };
        if (TransformVec3Row("pos", "Position", pos, 0.01f, 0.f, 0.f, "%.3f", kResetPos, &lockPos, false))
        {
            ew->setTranslation(entity, dm::double3(pos[0], pos[1], pos[2]));
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            m_settings.ResetAccumulation = true;
        }

        constexpr double deg2rad = 3.14159265358979323846 / 180.0;

        const bool selectedRotationEntityChanged = m_editorUI.InspectorRotationEntity != entity;
        if (!m_editorUI.InspectorRotationEulerValid || selectedRotationEntityChanged || !SameRotation(m_editorUI.InspectorRotationQuat, rotation))
        {
            m_editorUI.InspectorRotationEntity = entity;
            m_editorUI.InspectorRotationQuat = rotation;
            m_editorUI.InspectorRotationEulerDeg = QuaternionToEulerDegreesXYZ(rotation);
            m_editorUI.InspectorRotationEulerValid = true;
        }

        float euler[3] = {
            m_editorUI.InspectorRotationEulerDeg.x,
            m_editorUI.InspectorRotationEulerDeg.y,
            m_editorUI.InspectorRotationEulerDeg.z
        };
        constexpr float kResetRot[3] = { 0.f, 0.f, 0.f };
        if (TransformVec3Row("rot", "Rotation", euler, 0.5f, 0.f, 0.f, "%.2f", kResetRot, &lockRot, false))
        {
            m_editorUI.InspectorRotationEulerDeg = dm::float3(euler[0], euler[1], euler[2]);
            const dm::dquat newRotation = dm::rotationQuat(dm::double3(euler[0] * deg2rad, euler[1] * deg2rad, euler[2] * deg2rad));
            m_editorUI.InspectorRotationQuat = newRotation;
            ew->setRotation(entity, newRotation);
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            m_settings.ResetAccumulation = true;
        }

        float scl[3] = { float(scaling.x), float(scaling.y), float(scaling.z) };
        constexpr float kResetScl[3] = { 1.f, 1.f, 1.f };
        if (TransformVec3Row("scl", "Scale", scl, 0.01f, 0.001f, 1000.0f, "%.3f", kResetScl, &lockScl, true))
        {
            ew->setScaling(entity, dm::double3(scl[0], scl[1], scl[2]));
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            m_settings.ResetAccumulation = true;
        }
    }

    if (gaussianSplat)
    {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("3D Gaussian Splats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            if (InspectorDragFloat("Footprint Scale", &m_settings.GaussianSplatScale, 0.01f, 0.01f, 10.0f, "%.2f"))
            {
                if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
                    m_runtime.Invalidation.AccelerationStructRebuildRequested = true;
                m_settings.ResetAccumulation = true;
            }
            RESET_ON_CHANGE(InspectorDragFloat("Alpha", &m_settings.GaussianSplatAlphaScale, 0.01f, 0.0f, 4.0f, "%.2f"));
            RESET_ON_CHANGE(InspectorDragFloat("Brightness", &m_settings.GaussianSplatBrightness, 0.01f, 0.0f, 16.0f, "%.2f"));
            RESET_ON_CHANGE(InspectorColorEdit3("Tint Color", &m_settings.GaussianSplatTintColor.x));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RESET_ON_CHANGE(InspectorCheckbox("As Emitter", &m_settings.GaussianSplatAsEmitter));
            ImGui::BeginDisabled(!m_settings.GaussianSplatAsEmitter);
            RESET_ON_CHANGE(InspectorDragFloat(
                "Emission Intensity", &m_settings.GaussianSplatEmissionIntensity, 0.01f, 0.0f, 100.0f, "%.2f"));
            if (InspectorDragInt(
                    "Proxy Limit", &m_settings.GaussianSplatEmissionMaxProxyCount, 256.f, 0, 262144))
            {
                m_settings.GaussianSplatEmissionMaxProxyCount =
                    dm::clamp(m_settings.GaussianSplatEmissionMaxProxyCount, 0, 262144);
                m_settings.ResetAccumulation = true;
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RESET_ON_CHANGE(InspectorDragFloat(
                "Alpha Cull", &m_settings.GaussianSplatAlphaCullThreshold, 0.001f, 0.0f, 0.25f, "%.3f"));
            if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            {
                RESET_ON_CHANGE(InspectorDragFloat(
                    "Shadow Strength", &m_settings.GaussianSplatShadowStrength, 0.01f, 0.0f, 1.0f, "%.2f"));
            }
        }
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

void EditorUI::BuildMaterialEditorPanel(const PanelLayout& layout)
{
    if (!m_editorUI.ShowMaterialEditor)
        return;

    ImGui::Begin("Material Editor", &m_editorUI.ShowMaterialEditor);
    ImGui::PushItemWidth(layout.defItemWidth);

    std::shared_ptr<PTMaterial> material = PTMaterial::safeCast(m_editorUI.SelectedMaterial);
    auto* wr = caustica::editor::editorWorldRenderer(m_sceneEditor);
    auto materials = wr ? wr->lightingPasses().materials() : nullptr;
    if (material == nullptr || materials == nullptr)
    {
        ImGui::TextDisabled("No material selected");
        ImGui::PopItemWidth();
        ImGui::End();
        return;
    }

    ImGui::Text("Material %d: %s.%s ", material->gpuDataIndex, material->modelName.c_str(), material->name.c_str());

    const bool wasAlphaTestedEnabled = material->enableAlphaTesting;
    const bool wasTransmissionEnabled = material->enableTransmission;
    const bool wasExcludedFromNEE = material->excludeFromNEE;
    const float alphaCutoffBefore = material->alphaCutoff;
    const bool wasSkipRender = material->skipRender;

    MaterialShaderPermutationKey mspBefore = MaterialShaderPermutationKey(material->computeShaderPermutation(""));

    bool dirty = material->editorGui(*materials);

    MaterialShaderPermutationKey mspAfter = MaterialShaderPermutationKey(material->computeShaderPermutation(""));

    const float alphaCutoffAfter = material->alphaCutoff;

    if (mspBefore != mspAfter ||
        wasAlphaTestedEnabled != material->enableAlphaTesting ||
        wasTransmissionEnabled != material->enableTransmission ||
        wasExcludedFromNEE != material->excludeFromNEE ||
        wasSkipRender != material->skipRender ||
        dirty)
    {
        m_settings.ResetAccumulation = 1;
    }

    // UE-style: material edits must not force RTPSO CreateStateObject.
    // Alpha/skip/NEE visibility changes only need acceleration-structure refresh + SBT remap.
    if (wasAlphaTestedEnabled != material->enableAlphaTesting ||
        wasExcludedFromNEE != material->excludeFromNEE ||
        mspBefore != mspAfter ||
        wasSkipRender != material->skipRender)
    {
        m_runtime.Invalidation.AccelerationStructRebuildRequested = true;
    }
    (void)alphaCutoffBefore;
    (void)alphaCutoffAfter;

    if (m_runtime.Invalidation.AccelerationStructRebuildRequested)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Acceleration structure refresh scheduled.");
    else
        ImGui::Text(" ");

    ImGui::PopItemWidth();
    ImGui::End();
}


} // namespace caustica::editor

