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
#include <scene/SceneEcs.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/Passes/Debug/Korgi.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
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

void EditorUI::BuildInspectorPanel(const PanelLayout& layout)
{
    // Inspector panel: instance Transform + mesh name (right-click pick)
    auto scene = m_sceneEditor.GetScene();
    auto* ew = scene ? scene->GetEntityWorld() : nullptr;
    if (m_ui.SelectedEntity != ecs::NullEntity && m_ui.ShowInspector && ew)
    {
        ImGui::SetNextWindowPos(ImVec2(float(layout.scaledWidth) - 10.f, 10.f), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(layout.defWindowWidth, 0), ImGuiCond_Appearing);
        ImGui::Begin("Inspector");
        ImGui::PushItemWidth(layout.defItemWidth);

        const ecs::Entity entity = m_ui.SelectedEntity;
        std::string entityName = ew->getEntityName(entity);
        ImGui::Text("Node: %s", entityName.c_str());

        auto* meshComp = ew->world().tryGet<caustica::scene::MeshInstanceComponent>(entity);
        if (meshComp && meshComp->mesh)
            ImGui::Text("Mesh: %s", meshComp->mesh->name.c_str());
        auto* splatComp = ew->world().tryGet<caustica::scene::GaussianSplatComponent>(entity);
        const auto& gaussianSplat = splatComp ? splatComp->splat : nullptr;
        if (gaussianSplat)
        {
            ImGui::Text("Type: 3D Gaussian Splats");
            ImGui::Text("Splats: %u", gaussianSplat->loadedSplatCount);
            const std::string source = gaussianSplat->resolvedPath.empty() ? gaussianSplat->path : gaussianSplat->resolvedPath;
            ImGui::TextWrapped("Source: %s", source.c_str());
        }

        ImGui::Separator();

        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(entity);
            dm::double3 translation = ltc ? ltc->translation : dm::double3(0.0);
            dm::dquat rotation = ltc ? ltc->rotation : dm::dquat::identity();
            dm::double3 scaling = ltc ? ltc->scaling : dm::double3(1.0);

            float pos[3] = { float(translation.x), float(translation.y), float(translation.z) };
            if (ImGui::DragFloat3("Position", pos, 0.01f))
            {
                ew->setTranslation(entity, dm::double3(pos[0], pos[1], pos[2]));
                m_ui.ResetAccumulation = true;
            }

            constexpr double deg2rad = 3.14159265358979323846 / 180.0;

            const bool selectedRotationEntityChanged = m_ui.InspectorRotationEntity != entity;
            if (!m_ui.InspectorRotationEulerValid || selectedRotationEntityChanged || !SameRotation(m_ui.InspectorRotationQuat, rotation))
            {
                m_ui.InspectorRotationEntity = entity;
                m_ui.InspectorRotationQuat = rotation;
                m_ui.InspectorRotationEulerDeg = QuaternionToEulerDegreesXYZ(rotation);
                m_ui.InspectorRotationEulerValid = true;
            }

            float euler[3] = {
                m_ui.InspectorRotationEulerDeg.x,
                m_ui.InspectorRotationEulerDeg.y,
                m_ui.InspectorRotationEulerDeg.z
            };
            if (ImGui::DragFloat3("Rotation (deg)", euler, 0.5f, 0.0f, 360.0f, "%.1f"))
            {
                euler[0] = dm::clamp(euler[0], 0.0f, 360.0f);
                euler[1] = dm::clamp(euler[1], 0.0f, 360.0f);
                euler[2] = dm::clamp(euler[2], 0.0f, 360.0f);
                m_ui.InspectorRotationEulerDeg = dm::float3(euler[0], euler[1], euler[2]);
                const dm::dquat newRotation = dm::rotationQuat(dm::double3(euler[0] * deg2rad, euler[1] * deg2rad, euler[2] * deg2rad));
                m_ui.InspectorRotationQuat = newRotation;
                ew->setRotation(entity, newRotation);
                m_ui.ResetAccumulation = true;
            }

            float scl[3] = { float(scaling.x), float(scaling.y), float(scaling.z) };
            if (ImGui::DragFloat3("Scale", scl, 0.01f, 0.001f, 1000.0f))
            {
                ew->setScaling(entity, dm::double3(scl[0], scl[1], scl[2]));
                m_ui.ResetAccumulation = true;
            }
        }

        if (gaussianSplat)
        {
            if (ImGui::CollapsingHeader("3D Gaussian Splats", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox("Enabled", &gaussianSplat->enabled))
                {
                    m_ui.Invalidation.AccelerationStructRebuildRequested = true;
                    m_ui.ResetAccumulation = true;
                }
                if (ImGui::DragFloat("Footprint Scale", &m_ui.GaussianSplatScale, 0.01f, 0.01f, 10.0f, "%.2f"))
                {
                    if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
                        m_ui.Invalidation.AccelerationStructRebuildRequested = true;
                    m_ui.ResetAccumulation = true;
                }
                RESET_ON_CHANGE(ImGui::DragFloat("Alpha", &m_ui.GaussianSplatAlphaScale, 0.01f, 0.0f, 4.0f, "%.2f"));
                RESET_ON_CHANGE(ImGui::DragFloat("Brightness", &m_ui.GaussianSplatBrightness, 0.01f, 0.0f, 16.0f, "%.2f"));
                RESET_ON_CHANGE(ImGui::InputFloat3("Tint Color", (float*)&m_ui.GaussianSplatTintColor.x));

                RESET_ON_CHANGE(ImGui::Checkbox("As Emitter", &m_ui.GaussianSplatAsEmitter));
                ImGui::BeginDisabled(!m_ui.GaussianSplatAsEmitter);
                RESET_ON_CHANGE(ImGui::DragFloat("Emission Intensity", &m_ui.GaussianSplatEmissionIntensity, 0.01f, 0.0f, 100.0f, "%.2f"));
                if (ImGui::InputInt("Emission Proxy Limit", &m_ui.GaussianSplatEmissionMaxProxyCount, 256, 4096))
                {
                    m_ui.GaussianSplatEmissionMaxProxyCount = dm::clamp(m_ui.GaussianSplatEmissionMaxProxyCount, 0, 262144);
                    m_ui.ResetAccumulation = true;
                }
                ImGui::EndDisabled();

                RESET_ON_CHANGE(ImGui::DragFloat("Alpha Cull", &m_ui.GaussianSplatAlphaCullThreshold, 0.001f, 0.0f, 0.25f, "%.3f"));
                if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
                    RESET_ON_CHANGE(ImGui::DragFloat("Shadow Strength", &m_ui.GaussianSplatShadowStrength, 0.01f, 0.0f, 1.0f, "%.2f"));
            }
        }

        ImGui::PopItemWidth();
        ImGui::End();
    }


}

void EditorUI::BuildMaterialEditorPanel(const PanelLayout& layout)
{
    // Material Editor panel (right-click pick)
    std::shared_ptr<PTMaterial> material = PTMaterial::SafeCast(m_ui.SelectedMaterial);
    if (material != nullptr && m_sceneEditor.GetLightingPasses().materials() != nullptr && m_ui.ShowMaterialEditor)
    {
        const bool inspectorVisible = m_ui.SelectedEntity != ecs::NullEntity && m_ui.ShowInspector;
        ImGui::SetNextWindowPos(ImVec2(float(layout.scaledWidth) - 10.f, inspectorVisible ? 350.f : 10.f), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(layout.defWindowWidth, 0), ImGuiCond_Appearing);
        ImGui::Begin("Material Editor");
        ImGui::PushItemWidth(layout.defItemWidth);

        ImGui::Text("Material %d: %s.%s ", material->GPUDataIndex, material->ModelName.c_str(), material->Name.c_str());

        const bool wasAlphaTestedEnabled = material->EnableAlphaTesting;
        const bool wasTransmissionEnabled = material->EnableTransmission;
        const bool wasExcludedFromNEE = material->ExcludeFromNEE;
        const float alphaCutoffBefore = material->AlphaCutoff;
        const bool wasSkipRender = material->SkipRender;

        MaterialShaderPermutationKey mspBefore = MaterialShaderPermutationKey(material->ComputeShaderPermutation(""));

        bool dirty = material->EditorGUI(*m_sceneEditor.GetLightingPasses().materials());

        MaterialShaderPermutationKey mspAfter = MaterialShaderPermutationKey(material->ComputeShaderPermutation(""));

        const float alphaCutoffAfter = material->AlphaCutoff;

        if (mspBefore != mspAfter ||
            wasAlphaTestedEnabled != material->EnableAlphaTesting ||
            wasTransmissionEnabled != material->EnableTransmission ||
            wasExcludedFromNEE != material->ExcludeFromNEE ||
            wasSkipRender != material->SkipRender ||
            dirty)
        {
            if (auto s = m_sceneEditor.GetScene())
                s->RefreshSceneWorld(m_sceneEditor.GetFrameIndex());
            m_ui.ResetAccumulation = 1;
        }

        if (wasAlphaTestedEnabled != material->EnableAlphaTesting || alphaCutoffBefore != alphaCutoffAfter ||
            wasExcludedFromNEE != material->ExcludeFromNEE || mspBefore != mspAfter || wasSkipRender != material->SkipRender)
            m_ui.Invalidation.ShaderAndACRefreshDelayedRequest = 1.0f;

        if (m_ui.Invalidation.ShaderAndACRefreshDelayedRequest > 0)
            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "PLEASE NOTE: shader and AC rebuild scheduled!\nUI might freeze for a bit.");
        else
            ImGui::Text(" ");

        ImGui::PopItemWidth();
        ImGui::End();
    }


}


} // namespace caustica::editor

