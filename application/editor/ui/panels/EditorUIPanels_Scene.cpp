#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <core/path_utils.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
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

namespace
{
bool WouldRemoveLastEnvironmentLight(caustica::scene::SceneEntityWorld& entityWorld, ecs::Entity subtree)
{
    size_t environmentLightCount = 0;
    size_t removedEnvironmentLightCount = 0;
    entityWorld.world().each<caustica::scene::EnvironmentLightComponent>(
        [&](ecs::Entity light, caustica::scene::EnvironmentLightComponent&) {
            ++environmentLightCount;
            if (entityWorld.entitySubtreeContains(subtree, light))
                ++removedEnvironmentLightCount;
        });
    return environmentLightCount > 0 && removedEnvironmentLightCount == environmentLightCount;
}
} // namespace

void EditorUI::BuildScenePanel(const PanelLayout& layout)
{
    uint uncompressedTextureCount = (uint)m_sceneEditor.uncompressedTextures().size();
    if (uncompressedTextureCount > 0)
    {
        ImGui::TextColored(warnColor, "Scene has %d uncompressed textures", uncompressedTextureCount);
        if (ImGui::Button("Batch compress with nvtt_export.exe", { -1, 0 }))
            if (compressTextures(m_sceneEditor.uncompressedTextures()))
                caustica::setCurrentScene(*m_sceneEditor.app(), caustica::currentSceneName(*m_sceneEditor.app()), true);
    }

    if (m_sceneEditor.game() && m_sceneEditor.game()->IsInitialized())
    {
        if (ImGui::CollapsingHeader("Interactive elements"))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
            m_sceneEditor.game()->debugGUI(layout.indent);
        }
    }

    if (m_editorUI.TogglableNodes != nullptr && m_editorUI.TogglableNodes->size() > 0 && ImGui::CollapsingHeader("Togglables"))
    {
        for (int i = 0; i < m_editorUI.TogglableNodes->size(); i++)
        {
            auto& node = (*m_editorUI.TogglableNodes)[i];
            bool selected = node.IsSelected();
            if (ImGui::Checkbox(node.UIName.c_str(), &selected))
            {
                node.SetSelected(selected);
                m_settings.ResetAccumulation = true;
            }
        }
    }

    if (m_runtime.GaussianSplats.SplatCount > 0 && ImGui::CollapsingHeader("3D Gaussian Splats"))
    {
        RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

        RESET_ON_CHANGE(GaussianSplatPrimaryMethodCombo(m_ui));
        RESET_ON_CHANGE(ImGui::Checkbox("Mesh Depth Test", &m_settings.GaussianSplatDepthTest));
        ImGui::BeginDisabled(!m_settings.GaussianSplatDepthTest);
        RESET_ON_CHANGE(ImGui::Checkbox("Stable Mesh Edges", &m_settings.GaussianSplatDepthEdgeDilation));
        RESET_ON_CHANGE(ImGui::DragFloat(
            "Mesh Depth Bias", &m_settings.GaussianSplatDepthBias,
            1.0e-5f, 0.0f, 1.0e-2f, "%.6f"));
        ImGui::EndDisabled();

        RESET_ON_CHANGE(GaussianSplatShadowsModeCombo(m_ui));

        if (ImGui::CollapsingHeader("Rasterization", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

            RESET_ON_CHANGE(GaussianSplatSortingCombo(m_ui));
            RESET_ON_CHANGE(GaussianSplatFormatCombo("SH Format", &m_settings.GaussianSplatSHFormat));
            RESET_ON_CHANGE(ImGui::Checkbox("Mip splatting antialiasing", &m_settings.GaussianSplatMipAntialiasing));
            RESET_ON_CHANGE(ImGui::Checkbox("Quantize Normals", &m_settings.GaussianSplatQuantizeNormals));

            ImGui::SeparatorText("Culling");
            bool cullingChanged = false;
            cullingChanged |= ImGui::RadioButton("Disabled", &m_settings.GaussianSplatFrustumCulling, 0);
            cullingChanged |= ImGui::RadioButton("At distance stage", &m_settings.GaussianSplatFrustumCulling, 1);
            cullingChanged |= ImGui::RadioButton("At raster stage", &m_settings.GaussianSplatFrustumCulling, 2);
            RESET_ON_CHANGE(cullingChanged);
            RESET_ON_CHANGE(ImGui::DragFloat("Frustum dilation", &m_settings.GaussianSplatFrustumDilation, 0.01f, 0.0f, 1.0f, "%.2f"));
            RESET_ON_CHANGE(ImGui::Checkbox("Screen size culling", &m_settings.GaussianSplatScreenSizeCulling));
            ImGui::BeginDisabled(!m_settings.GaussianSplatScreenSizeCulling);
            RESET_ON_CHANGE(ImGui::DragFloat("Min pixel coverage", &m_settings.GaussianSplatMinPixelCoverage, 0.1f, 0.1f, 20.0f, "%.2f"));
            ImGui::EndDisabled();
        }

        if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED
            && ImGui::CollapsingHeader("Mesh Shadow RT", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
            ImGui::TextDisabled("Shadows use mesh/Gaussian BVH rays; primary stays 3DGS/3DGUT.");

            bool asChanged = false;
            asChanged |= GaussianSplatShadowKernelDegreeCombo(m_ui);
            asChanged |= GaussianSplatShadowParticleFormatCombo(m_ui);
            asChanged |= ImGui::Checkbox("Adaptive clamp", &m_settings.GaussianSplatShadowAdaptiveClamp);
            if (asChanged)
            {
                m_runtime.Invalidation.AccelerationStructRebuildRequested = true;
                m_settings.ResetAccumulation = true;
            }

            if (ResolveGaussianSplatShadowMode(m_ui) == GAUSSIAN_SPLAT_SHADOWS_SOFT)
            {
                RESET_ON_CHANGE(ImGui::DragFloat("Soft shadow radius", &m_settings.GaussianSplatShadowSoftRadius, 0.01f, 0.0f, 0.5f, "%.2f"));
                RESET_ON_CHANGE(ImGui::InputInt("Soft shadow samples", &m_settings.GaussianSplatShadowSoftSampleCount, 1, 4));
                m_settings.GaussianSplatShadowSoftSampleCount = dm::clamp(m_settings.GaussianSplatShadowSoftSampleCount, 1, 16);
            }

            RESET_ON_CHANGE(ImGui::DragFloat("Ray offset", &m_settings.GaussianSplatShadowRayOffset, 0.01f, 0.0f, 1.0f, "%.2f"));
        }
    }
}

void EditorUI::BuildSampleGamePanel(const PanelLayout& layout)
{
        if (m_sceneEditor.game() && m_sceneEditor.game()->IsInitialized())
        {
            if (ImGui::CollapsingHeader("Sample Game"/*, ImGuiTreeNodeFlags_DefaultOpen*/))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                m_sceneEditor.game()->debugGUI(layout.indent);
            }
        }


}

void EditorUI::BuildSceneWidgetsPanel(const PanelLayout& layout)
{
    if (m_showSceneWidgets > 0.0f 
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
        && !m_editorUI.ShowDeltaTree
#endif
        )
    {

        // collect toggles
        struct BigButton
        {
            std::string Name;
            TogglableNode* PropNode = nullptr;
            bool enabled = true;

            BigButton(const std::string& name, TogglableNode* prop)
                : Name(TrimTogglable(name)), PropNode(prop)
            {}
            bool IsSelected() const { return PropNode->IsSelected(); }
            void SetSelected(bool selected) { PropNode->SetSelected(selected); }
        };
        std::vector<BigButton> buttons;
        for (int i = 0; m_editorUI.TogglableNodes != nullptr && i < m_editorUI.TogglableNodes->size(); i++)
            buttons.push_back(BigButton((*m_editorUI.TogglableNodes)[i].UIName, &(*m_editorUI.TogglableNodes)[i]));

        if( buttons.size() > 0 )
        {
            // show & 
            ImVec2 texSizeA = ImGui::CalcTextSize("A");
            float buttonWidth = texSizeA.x * 16;
            float windowHeight = texSizeA.y * 3.0f;
            float windowWidth = buttonWidth * buttons.size() + ImGui::GetStyle().ItemSpacing.x * (buttons.size()+1);
            ImGui::SetNextWindowPos(ImVec2(0.5f * (layout.scaledWidth - windowWidth), 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0f);
            if (ImGui::Begin("Widgets", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav))
            {
                for (int i = 0; i < buttons.size(); i++)
                {
                    if (i > 0)
                        ImGui::SameLine();
                    
                    UI_SCOPED_DISABLE(!buttons[i].enabled);

                    bool selected = buttons[i].IsSelected();

                    ImGui::PushID(i);
                    PushToolbarButtonColors(selected);
                    if (ImGui::Button(buttons[i].Name.c_str(), ImVec2(buttonWidth, texSizeA.y * 2)))
                    {
                        buttons[i].SetSelected(!selected);
                        m_settings.ResetAccumulation = true;
                    }
                    PopToolbarButtonColors();
                    ImGui::PopID();
                }
            }
            ImGui::End();
        }
    }


}

void EditorUI::BuildHierarchyPanel(const PanelLayout& layout)
{
    {
        (void)layout;
        RAII_SCOPE(ImGui::Begin("Hierarchy", &m_editorUI.Viewport.ShowHierarchy, ImGuiWindowFlags_None);, ImGui::End(););

        auto scene = caustica::activeScene(*m_sceneEditor.app());
        auto* ew = caustica::entityWorld(*m_sceneEditor.app());

        if (scene && ew && ew->root() != ecs::NullEntity)
        {
            bool deleteSelectedEntity = false;

            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputTextWithHint(
                "##HierarchySearch",
                "Search entities...",
                m_editorUI.Viewport.HierarchyFilter,
                IM_ARRAYSIZE(m_editorUI.Viewport.HierarchyFilter));
            ImGui::Spacing();

            // Entity ids are reused by each imported ECS world. Namespace the
            // ImGui tree state by scene path so expansion state cannot leak from
            // Kitchen (or another scene) into Bistro after Open Scene.
            const std::string hierarchySceneId =
                caustica::currentScenePath(*m_sceneEditor.app()).generic_string();
            ImGui::PushID(hierarchySceneId.c_str());
            if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                BuildHierarchyNodeUI(m_ui, *scene, ew->root(), m_editorUI.Viewport.HierarchyFilter);
                ImGui::TreePop();
            }
            ImGui::PopID();

            const ecs::Entity selected = m_editorUI.SelectedEntity;
            const bool selectedAlive = selected != ecs::NullEntity && ew->world().isAlive(selected);
            const bool canDeleteSelected = selectedAlive
                && selected != ew->root()
                && !WouldRemoveLastEnvironmentLight(*ew, selected)
                && m_editorUI.PendingDeleteEntity == ecs::NullEntity;
            // Allow Delete after viewport pick too (not only when Hierarchy is focused).
            // Disable key-repeat so holding Delete cannot queue overlapping deletes.
            const bool deleteKeyPressed = !ImGui::GetIO().WantTextInput
                && ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false);
            if (canDeleteSelected && deleteKeyPressed)
                deleteSelectedEntity = true;

            if (deleteSelectedEntity)
            {
                // Defer destruction to the main thread. Mutating the scene from the UI/render
                // thread races with pipelined main-thread update/Extract and can crash.
                m_editorUI.PendingDeleteEntity = selected;
                m_editorUI.SelectedEntity = ecs::NullEntity;
                m_editorUI.SelectedMaterial = nullptr;
                m_editorUI.InspectorRotationEntity = ecs::NullEntity;
                m_editorUI.InspectorRotationEulerValid = false;
                m_editorUI.SelectedGaussianSplat = false;
            }
        }
        else
        {
            ImGui::TextDisabled("No scene loaded.");
        }
    }


}


} // namespace caustica::editor
