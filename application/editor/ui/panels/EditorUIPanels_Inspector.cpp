#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUndoCommands.h"
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include "common/ImGuiManager.h"
#include "common/TransformGizmo.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <core/path_utils.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/SceneCameraAccess.h>
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

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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

    // Function-static edit session for Transform undo grouping (see Transform section).
    struct InspectorTransformUndoState
    {
        bool tracking = false;
        bool dirty = false;
        ecs::Entity entity = ecs::NullEntity;
        LocalTransformSnapshot before;
    };
    static InspectorTransformUndoState s_transformUndo;

    if (!ew || m_editorUI.SelectedEntity == ecs::NullEntity)
    {
        s_transformUndo = {};
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
    auto* cameraComp = caustica::scene::tryGetCamera(ew->world(), entity);
    auto* envLightComp = caustica::scene::tryGetEnvironmentLight(ew->world(), entity);
    auto* dirLightComp = caustica::scene::tryGetDirectionalLight(ew->world(), entity);
    auto* spotLightComp = caustica::scene::tryGetSpotLight(ew->world(), entity);
    auto* pointLightComp = caustica::scene::tryGetPointLight(ew->world(), entity);
    caustica::GaussianSplat* gaussianSplat = splatComp ? &splatComp->splat : nullptr;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));

    ImGui::TextUnformatted(entityName.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    if (meshComp && meshComp->mesh)
        ImGui::Text("Mesh · %s", meshComp->mesh->name.c_str());
    else if (gaussianSplat)
        ImGui::Text("3DGS · %u splats", gaussianSplat->loadedSplatCount);
    else if (cameraComp)
        ImGui::TextUnformatted(caustica::scene::isPerspectiveCamera(*cameraComp) ? "Camera · Perspective" : "Camera · Orthographic");
    else if (envLightComp)
        ImGui::TextUnformatted("Light · Environment");
    else if (dirLightComp)
        ImGui::TextUnformatted("Light · Directional");
    else if (spotLightComp)
        ImGui::TextUnformatted("Light · Spot");
    else if (pointLightComp)
        ImGui::TextUnformatted("Light · Point");
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
        const LocalTransformSnapshot frameBefore = ltc
            ? captureLocalTransform(*ltc)
            : LocalTransformSnapshot{};

        static bool lockPos = false;
        static bool lockRot = false;
        static bool lockScl = false;

        auto beginTransformEdit = [&](const TransformVec3RowEditInfo& edit)
        {
            if (!edit.activated || s_transformUndo.tracking)
                return;
            s_transformUndo.tracking = true;
            s_transformUndo.dirty = false;
            s_transformUndo.entity = entity;
            s_transformUndo.before = frameBefore;
        };
        auto markTransformDirty = [&]()
        {
            if (s_transformUndo.tracking)
                s_transformUndo.dirty = true;
        };
        auto endTransformEdit = [&](const TransformVec3RowEditInfo& edit)
        {
            if (!edit.deactivated || !s_transformUndo.tracking)
                return;
            if (s_transformUndo.dirty || edit.deactivatedAfterEdit)
            {
                const LocalTransformSnapshot after = captureLocalTransform(*ew, s_transformUndo.entity);
                m_sceneEditor.commitTransformEdit(s_transformUndo.entity, s_transformUndo.before, after);
            }
            s_transformUndo = {};
        };
        if (s_transformUndo.tracking && s_transformUndo.entity != entity)
            s_transformUndo = {};

        float pos[3] = { float(translation.x), float(translation.y), float(translation.z) };
        constexpr float kResetPos[3] = { 0.f, 0.f, 0.f };
        TransformVec3RowEditInfo posEdit;
        if (TransformVec3Row("pos", "Position", pos, 0.01f, 0.f, 0.f, "%.3f", kResetPos, &lockPos, false, &posEdit))
        {
            beginTransformEdit(posEdit);
            ew->setTranslation(entity, dm::double3(pos[0], pos[1], pos[2]));
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            markTransformDirty();
            m_settings.ResetAccumulation = true;
        }
        beginTransformEdit(posEdit);
        endTransformEdit(posEdit);

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
        TransformVec3RowEditInfo rotEdit;
        if (TransformVec3Row("rot", "Rotation", euler, 0.5f, 0.f, 0.f, "%.2f", kResetRot, &lockRot, false, &rotEdit))
        {
            beginTransformEdit(rotEdit);
            m_editorUI.InspectorRotationEulerDeg = dm::float3(euler[0], euler[1], euler[2]);
            const dm::dquat newRotation = dm::rotationQuat(dm::double3(euler[0] * deg2rad, euler[1] * deg2rad, euler[2] * deg2rad));
            m_editorUI.InspectorRotationQuat = newRotation;
            ew->setRotation(entity, newRotation);
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            markTransformDirty();
            m_settings.ResetAccumulation = true;
        }
        beginTransformEdit(rotEdit);
        endTransformEdit(rotEdit);

        float scl[3] = { float(scaling.x), float(scaling.y), float(scaling.z) };
        constexpr float kResetScl[3] = { 1.f, 1.f, 1.f };
        TransformVec3RowEditInfo sclEdit;
        if (TransformVec3Row("scl", "Scale", scl, 0.01f, 0.001f, 1000.0f, "%.3f", kResetScl, &lockScl, true, &sclEdit))
        {
            beginTransformEdit(sclEdit);
            ew->setScaling(entity, dm::double3(scl[0], scl[1], scl[2]));
            ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
            markTransformDirty();
            m_settings.ResetAccumulation = true;
        }
        beginTransformEdit(sclEdit);
        endTransformEdit(sclEdit);
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

    if (cameraComp)
    {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            if (auto* pers = caustica::scene::tryGetPerspectiveCameraData(*cameraComp))
            {
                float fovDeg = dm::degrees(pers->verticalFov);
                if (InspectorDragFloat("Vertical FOV", &fovDeg, 0.1f, 1.f, 179.f, "%.1f°"))
                {
                    pers->verticalFov = dm::radians(dm::clamp(fovDeg, 1.f, 179.f));
                    m_settings.ResetAccumulation = true;
                }
                RESET_ON_CHANGE(InspectorDragFloat("Near Clip", &pers->zNear, 0.01f, 0.001f, 1e6f, "%.3f"));
                if (pers->zFar.has_value())
                {
                    float zFar = *pers->zFar;
                    if (InspectorDragFloat("Far Clip", &zFar, 1.f, pers->zNear + 0.01f, 1e7f, "%.1f"))
                    {
                        pers->zFar = zFar;
                        m_settings.ResetAccumulation = true;
                    }
                }
            }
            else if (auto* ortho = caustica::scene::tryGetOrthographicCameraData(*cameraComp))
            {
                RESET_ON_CHANGE(InspectorDragFloat("Near Clip", &ortho->zNear, 0.01f, 0.f, 1e6f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Far Clip", &ortho->zFar, 1.f, ortho->zNear + 0.01f, 1e7f, "%.1f"));
                RESET_ON_CHANGE(InspectorDragFloat("X Mag", &ortho->xMag, 0.01f, 0.001f, 1e6f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Y Mag", &ortho->yMag, 0.01f, 0.001f, 1e6f, "%.3f"));
            }

            ImGui::Spacing();
            auto scene = caustica::activeScene(*m_sceneEditor.app());
            static const std::vector<ecs::Entity> kNoCameras;
            const auto& cameraEntities = scene ? scene->getCameraEntities() : kNoCameras;
            int sceneCamIndex = -1;
            for (size_t i = 0; i < cameraEntities.size(); ++i)
            {
                if (cameraEntities[i] == entity)
                {
                    sceneCamIndex = static_cast<int>(i);
                    break;
                }
            }
            uint& activeCam = caustica::selectedCameraIndex(*m_sceneEditor.app());
            const bool isActive = sceneCamIndex >= 0 && activeCam == static_cast<uint>(sceneCamIndex + 1);
            ImGui::BeginDisabled(isActive || sceneCamIndex < 0);
            if (ImGui::Button("Set as Active Camera", ImVec2(-1.f, 0.f)) && sceneCamIndex >= 0)
                activeCam = static_cast<uint>(sceneCamIndex + 1);
            ImGui::EndDisabled();
            if (isActive)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
                ImGui::TextUnformatted("Active viewport camera");
                ImGui::PopStyleColor();
            }
        }
    }

    if (envLightComp)
    {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Environment Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
            if (isProceduralSky(caustica::envMapLocalPath(*m_sceneEditor.app()).c_str()) ||
                isProceduralSky(caustica::envMapOverrideSource(*m_sceneEditor.app()).c_str()))
                ImGui::TextWrapped("Source: Procedural Sky (Hillaire 2020)");
            else if (!envLightComp->path.empty())
                ImGui::TextWrapped("Scene path: %s", envLightComp->path.c_str());
            else
                ImGui::TextWrapped("Source: `%s`", caustica::envMapLocalPath(*m_sceneEditor.app()).c_str());
            ImGui::PopStyleColor();

            // Live controls bind to the session EnvironmentMapParams (same path the
            // renderer consumes). Scene JSON EnvironmentLight fields seed load-time state.
            RESET_ON_CHANGE(InspectorCheckbox("Enabled", &m_settings.EnvironmentMapParams.enabled));
            RESET_ON_CHANGE(InspectorCheckbox("Visible to Camera", &m_settings.EnvironmentMapParams.VisibleToCamera));

            std::string overrideSource = caustica::envMapOverrideSource(*m_sceneEditor.app());
            const std::vector<std::filesystem::path>& envMapMediaList = caustica::envMapMediaList(*m_sceneEditor.app());
            const std::string overridePreview = isProceduralSky(overrideSource.c_str()) || overrideSource == c_EnvMapSceneDefault
                ? TrimSkyDisplayName(overrideSource)
                : overrideSource;
            if (InspectorBeginCombo("Override", overridePreview.c_str()))
            {
                for (int i = -7; i < (int)envMapMediaList.size(); i++)
                {
                    std::string itemName;
                    if (i == -7)
                        itemName = c_EnvMapSceneDefault;
                    else if (i == -6)
                        itemName = c_EnvMapProcSky;
                    else if (i == -5)
                        itemName = c_EnvMapProcSky_Morning;
                    else if (i == -4)
                        itemName = c_EnvMapProcSky_Midday;
                    else if (i == -3)
                        itemName = c_EnvMapProcSky_Evening;
                    else if (i == -2)
                        itemName = c_EnvMapProcSky_Dawn;
                    else if (i == -1)
                        itemName = c_EnvMapProcSky_PitchBlack;
                    else
                        itemName = envMapMediaList[i].filename().string();

                    const std::string displayName = (i < 0) ? TrimSkyDisplayName(itemName) : itemName;
                    const bool is_selected = itemName == overrideSource;
                    if (ImGui::Selectable(displayName.c_str(), is_selected))
                        overrideSource = itemName;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
                ImGui::PopID();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Overrides scene environment map.\n"
                    "'sky (manual)' = free elevation/azimuth control in Sky Atmosphere.");
            if (caustica::envMapOverrideSource(*m_sceneEditor.app()) != overrideSource)
            {
                m_settings.ResetAccumulation = true;
                caustica::setEnvMapOverrideSource(*m_sceneEditor.app(), overrideSource);
            }

            RESET_ON_CHANGE(InspectorColorEdit3("Tint Color", &m_settings.EnvironmentMapParams.TintColor.x));
            RESET_ON_CHANGE(InspectorDragFloat("Intensity", &m_settings.EnvironmentMapParams.Intensity, 0.01f, 0.f, 100.f, "%.3f"));
            RESET_ON_CHANGE(InspectorDragFloat3(
                "Rotation XYZ", &m_settings.EnvironmentMapParams.RotationXYZ.x, 0.5f, -360.f, 360.f, "%.1f"));

            if (auto& envMapProcessor = caustica::editor::requireWorldRenderer(m_sceneEditor).lightingPasses().environment();
                envMapProcessor != nullptr && envMapProcessor->isProcedural() && envMapProcessor->getProceduralSky() != nullptr)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(categoryColor, "Sky Atmosphere");
                m_settings.ResetAccumulation |= envMapProcessor->getProceduralSky()->debugGUI(layout.indent);
            }
        }
    }

    if (dirLightComp || spotLightComp || pointLightComp)
    {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            if (dirLightComp)
            {
                RESET_ON_CHANGE(InspectorColorEdit3("Color", &dirLightComp->color.x));
                RESET_ON_CHANGE(InspectorDragFloat("Irradiance", &dirLightComp->irradiance, 0.01f, 0.f, 1000.f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Angular Size", &dirLightComp->angularSize, 0.01f, 0.f, 90.f, "%.2f"));
            }
            else if (spotLightComp)
            {
                RESET_ON_CHANGE(InspectorColorEdit3("Color", &spotLightComp->color.x));
                RESET_ON_CHANGE(InspectorDragFloat("Intensity", &spotLightComp->intensity, 0.01f, 0.f, 1e6f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Radius", &spotLightComp->radius, 0.01f, 0.f, 100.f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Range", &spotLightComp->range, 0.1f, 0.f, 1e6f, "%.2f"));
                RESET_ON_CHANGE(InspectorDragFloat("Inner Angle", &spotLightComp->innerAngle, 0.5f, 0.f, 180.f, "%.1f"));
                RESET_ON_CHANGE(InspectorDragFloat("Outer Angle", &spotLightComp->outerAngle, 0.5f, 0.f, 180.f, "%.1f"));
            }
            else if (pointLightComp)
            {
                RESET_ON_CHANGE(InspectorColorEdit3("Color", &pointLightComp->color.x));
                RESET_ON_CHANGE(InspectorDragFloat("Intensity", &pointLightComp->intensity, 0.01f, 0.f, 1e6f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Radius", &pointLightComp->radius, 0.01f, 0.f, 100.f, "%.3f"));
                RESET_ON_CHANGE(InspectorDragFloat("Range", &pointLightComp->range, 0.1f, 0.f, 1e6f, "%.2f"));
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

    std::shared_ptr<StandardMaterial> material = StandardMaterial::safeCast(m_editorUI.SelectedMaterial);
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

