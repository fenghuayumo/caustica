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

bool EditorUI::BuildSceneComboPanel(const PanelLayout& layout)
{
    bool sceneChangeRequested = false;
    std::string requestedScene;

    {
        const std::string currentScene = m_sceneEditor.GetCurrentSceneName();
        RAII_SCOPE(ImGui::PushItemWidth(-60.0f * m_currentScale); , ImGui::PopItemWidth(); );
        RAII_SCOPE(ImGui::PushID("SceneComboID"); , ImGui::PopID(); );
        if (ImGui::BeginCombo("Scene", currentScene.c_str()))
        {
            const std::vector<std::string>& scenes = m_sceneEditor.GetAvailableScenes();
            for (const std::string& scene : scenes)
            {
                bool is_selected = scene == currentScene;
                if (ImGui::Selectable(scene.c_str(), is_selected) && !is_selected)
                {
                    requestedScene = scene;
                    sceneChangeRequested = true;
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (sceneChangeRequested)
    {
        m_sceneEditor.SetCurrentScene(requestedScene);
        return true;
    }

    return false;
}

void EditorUI::BuildScenePanel(const PanelLayout& layout)
{
        if (ImGui::CollapsingHeader("Scene"/*, ImGuiTreeNodeFlags_DefaultOpen*/))
        {
            RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
            uint uncompressedTextureCount = (uint)m_sceneEditor.GetUncompressedTextures().size();
            if (uncompressedTextureCount > 0)
            {
                ImGui::TextColored(warnColor, "Scene has %d uncompressed textures", uncompressedTextureCount);
                if (ImGui::Button("Batch compress with nvtt_export.exe", { -1, 0 }))
                    if (CompressTextures(m_sceneEditor.GetUncompressedTextures()))
                    {   // reload scene
                        m_sceneEditor.SetCurrentScene(m_sceneEditor.GetCurrentSceneName(), true);
                    }
            }

            {
                UI_SCOPED_DISABLE(!m_ui.RealtimeMode);
                ImGui::Checkbox("Enable animations", &m_ui.EnableAnimations);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Animations are not available in reference mode");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset animation time"))
            {
                m_sceneEditor.SetSceneTime(0);
                m_ui.ResetAccumulation = true;
            }

            if (m_sceneEditor.GetGame() && m_sceneEditor.GetGame()->IsInitialized())
            {
                if (ImGui::CollapsingHeader("Interactive elements"/*, ImGuiTreeNodeFlags_DefaultOpen*/))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                    m_sceneEditor.GetGame()->DebugGUI(layout.indent);
                }
            }

            if (m_ui.TogglableNodes != nullptr && m_ui.TogglableNodes->size() > 0 && ImGui::CollapsingHeader("Togglables"))
            {
                for (int i = 0; i < m_ui.TogglableNodes->size(); i++)
                {
                    auto& node = (*m_ui.TogglableNodes)[i];
                    bool selected = node.IsSelected();
                    if (ImGui::Checkbox(node.UIName.c_str(), &selected))
                    {
                        node.SetSelected(selected);
                        m_ui.ResetAccumulation = true;
                    }
                }
            }

            if (m_ui.GaussianSplats.SplatCount > 0 && ImGui::CollapsingHeader("3D Gaussian Splats"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                RESET_ON_CHANGE(ImGui::Checkbox("Mesh Depth Test", &m_ui.GaussianSplatDepthTest));
                GaussianSplatModeCombo(m_ui);
                GaussianSplatShadowsModeCombo(m_ui);

                if (ImGui::CollapsingHeader("Rasterization", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                    RESET_ON_CHANGE(GaussianSplatSortingCombo(m_ui));
                    RESET_ON_CHANGE(ImGui::Checkbox("Mip splatting antialiasing", &m_ui.GaussianSplatMipAntialiasing));
                    RESET_ON_CHANGE(ImGui::Checkbox("Quantize Normals", &m_ui.GaussianSplatQuantizeNormals));
                    RESET_ON_CHANGE(GaussianSplatFTBCombo(m_ui));
                    RESET_ON_CHANGE(ImGui::DragFloat("Depth Iso Threshold", &m_ui.GaussianSplatDepthIsoThreshold, 0.01f, 0.0f, 1.0f, "%.2f"));
                    RESET_ON_CHANGE(ImGui::Checkbox("Fragment shader barycentric", &m_ui.GaussianSplatFragmentShaderBarycentric));

                    ImGui::SeparatorText("Culling");
                    bool cullingChanged = false;
                    cullingChanged |= ImGui::RadioButton("Disabled", &m_ui.GaussianSplatFrustumCulling, 0);
                    cullingChanged |= ImGui::RadioButton("At distance stage", &m_ui.GaussianSplatFrustumCulling, 1);
                    cullingChanged |= ImGui::RadioButton("At raster stage", &m_ui.GaussianSplatFrustumCulling, 2);
                    RESET_ON_CHANGE(cullingChanged);
                    RESET_ON_CHANGE(ImGui::DragFloat("Frustum dilation", &m_ui.GaussianSplatFrustumDilation, 0.01f, 0.0f, 1.0f, "%.2f"));
                    RESET_ON_CHANGE(ImGui::Checkbox("Screen size culling", &m_ui.GaussianSplatScreenSizeCulling));
                    ImGui::BeginDisabled(!m_ui.GaussianSplatScreenSizeCulling);
                    RESET_ON_CHANGE(ImGui::DragFloat("Min pixel coverage", &m_ui.GaussianSplatMinPixelCoverage, 0.1f, 0.1f, 20.0f, "%.2f"));
                    ImGui::EndDisabled();
                }

                if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                    RESET_ON_CHANGE(ImGui::Checkbox("As Emitter", &m_ui.GaussianSplatAsEmitter));
                    ImGui::BeginDisabled(!m_ui.GaussianSplatAsEmitter);
                    RESET_ON_CHANGE(ImGui::DragFloat("Emission Intensity", &m_ui.GaussianSplatEmissionIntensity, 0.01f, 0.0f, 100.0f, "%.2f"));
                    if (ImGui::InputInt("Emission Proxy Limit", &m_ui.GaussianSplatEmissionMaxProxyCount, 256, 4096))
                    {
                        m_ui.GaussianSplatEmissionMaxProxyCount = dm::clamp(m_ui.GaussianSplatEmissionMaxProxyCount, 0, 262144);
                        m_ui.ResetAccumulation = true;
                    }
                    ImGui::EndDisabled();
                }

                if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED
                    && ImGui::CollapsingHeader("Ray Tracing", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                    bool asChanged = false;
                    asChanged |= GaussianSplatRtxKernelDegreeCombo(m_ui);
                    asChanged |= GaussianSplatRtxParticleFormatCombo(m_ui);
                    asChanged |= ImGui::Checkbox("Adaptive clamp", &m_ui.GaussianSplatRtxAdaptiveClamp);
                    if (asChanged)
                    {
                        m_ui.Invalidation.AccelerationStructRebuildRequested = true;
                        m_ui.ResetAccumulation = true;
                    }

                    if (ResolveGaussianSplatShadowMode(m_ui) == GAUSSIAN_SPLAT_SHADOWS_SOFT)
                    {
                        RESET_ON_CHANGE(ImGui::DragFloat("Soft shadow radius", &m_ui.GaussianSplatShadowSoftRadius, 0.01f, 0.0f, 0.5f, "%.2f"));
                        RESET_ON_CHANGE(ImGui::InputInt("Soft shadow samples", &m_ui.GaussianSplatShadowSoftSampleCount, 1, 4));
                        m_ui.GaussianSplatShadowSoftSampleCount = dm::clamp(m_ui.GaussianSplatShadowSoftSampleCount, 1, 16);
                    }

                    RESET_ON_CHANGE(ImGui::DragFloat("Ray offset", &m_ui.GaussianSplatRtxParticleShadowOffset, 0.01f, 0.0f, 1.0f, "%.2f"));
                }
            }

            if (ImGui::CollapsingHeader("Environment Map"))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );

                RESET_ON_CHANGE(ImGui::Checkbox("Enabled", &m_ui.EnvironmentMapParams.Enabled));
                RESET_ON_CHANGE(ImGui::Checkbox("Visible to Camera", &m_ui.EnvironmentMapParams.VisibleToCamera));

                if (m_sceneEditor.GetEnvMapLocalPath() != "==PROCEDURAL_SKY==")
                    ImGui::TextWrapped("Source: `%s`", m_sceneEditor.GetEnvMapLocalPath().c_str());
                else
                    ImGui::TextWrapped("Source: Procedural Sky");

                std::string overrideSource = m_sceneEditor.GetEnvMapOverrideSource();
                const std::vector<std::filesystem::path> & envMapMediaList = m_sceneEditor.GetEnvMapMediaList();

                RAII_SCOPE( ImGui::PushItemWidth(-65.0f*m_currentScale);, ImGui::PopItemWidth(); );
                if (ImGui::BeginCombo("Override", overrideSource.c_str()))
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

                        bool is_selected = itemName == overrideSource;
                        if (ImGui::Selectable(itemName.c_str(), is_selected))
                            overrideSource = itemName;
                        if (is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overrides scene's default environment map");
                if (m_sceneEditor.GetEnvMapOverrideSource() != overrideSource)
                {
                    m_ui.ResetAccumulation = true;
                    m_sceneEditor.SetEnvMapOverrideSource(overrideSource);
                }

                ImGui::Separator();
                RESET_ON_CHANGE( ImGui::InputFloat3("Tint Color", (float*)&m_ui.EnvironmentMapParams.TintColor.x) );
                RESET_ON_CHANGE( ImGui::InputFloat("Intensity", &m_ui.EnvironmentMapParams.Intensity) );
                RESET_ON_CHANGE( ImGui::InputFloat3("Rotation XYZ", (float*)&m_ui.EnvironmentMapParams.RotationXYZ.x) );
                ImGui::Separator();

                if (auto& envMapProcessor = m_sceneEditor.GetLightingPasses().environment();
                    envMapProcessor != nullptr && envMapProcessor->IsProcedural() && envMapProcessor->GetProceduralSky() != nullptr)
                {
                    ImGui::TextColored(categoryColor, "Procedural Sky settings:");
                    RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent););
                    envMapProcessor->GetProceduralSky()->DebugGUI(layout.indent);
                }
            }

            if (ImGui::CollapsingHeader("Materials"))
            {
                RAII_SCOPE( ImGui::Indent(layout.indent);, ImGui::Unindent(layout.indent); );
                if (auto& materialGpuCache = m_sceneEditor.GetLightingPasses().materials(); materialGpuCache != nullptr)
                    materialGpuCache->DebugGUI(layout.indent);
            }
        }


}

void EditorUI::BuildSampleGamePanel(const PanelLayout& layout)
{
        if (m_sceneEditor.GetGame() && m_sceneEditor.GetGame()->IsInitialized())
        {
            if (ImGui::CollapsingHeader("Sample Game"/*, ImGuiTreeNodeFlags_DefaultOpen*/))
            {
                RAII_SCOPE(ImGui::Indent(layout.indent); , ImGui::Unindent(layout.indent); );
                m_sceneEditor.GetGame()->DebugGUI(layout.indent);
            }
        }


}

void EditorUI::BuildSceneWidgetsPanel(const PanelLayout& layout)
{
    if (m_showSceneWidgets > 0.0f 
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
        && !m_ui.ShowDeltaTree
#endif
        )
    {

        std::string envMapOverrideSource = m_sceneEditor.GetEnvMapOverrideSource();
        std::vector<std::string> envOptions;
        envOptions.push_back( c_EnvMapSceneDefault );
        //envOptions.push_back( c_EnvMapProcSky );
        envOptions.push_back( c_EnvMapProcSky_Morning );
        envOptions.push_back( c_EnvMapProcSky_Midday );
        envOptions.push_back( c_EnvMapProcSky_Evening );
        envOptions.push_back( c_EnvMapProcSky_Dawn );
        envOptions.push_back( c_EnvMapProcSky_PitchBlack );
        int envOptionsCurrentIndex = -1; for (int i = 0; i < envOptions.size(); i++) if (envOptions[i]==envMapOverrideSource) envOptionsCurrentIndex = i;

        std::vector<std::string> materialVariants;
        if (FindSubStringIgnoreCase(m_sceneEditor.GetCurrentSceneName(), "bistro") != std::string::npos)
            materialVariants = {"dry", "wet", "silly"};
        int materialVariantIndexPrev = m_ui.MaterialVariantIndex;

        // collect toggles
        struct BigButton
        {
            std::string                 Name;
            std::optional<std::string>  HoverText;

            bool *                      PropVar         = nullptr; // type 1
            TogglableNode *             PropNode        = nullptr; // type 2
            std::vector<std::string> *  PropOptions     = nullptr; // type 3
            int *                       PropOptionIndex = nullptr; // type 3
            std::function<std::string(std::string)>
                                        GetItemName     = nullptr;

            bool                        Enabled;

            BigButton( const std::string & name, bool & prop ) : Name(name), PropVar(&prop), PropNode(nullptr), Enabled(true) {}
            BigButton( const std::string & name, bool & prop, const std::string& hoverText, bool enabled ) : Name(name), PropVar(&prop), PropNode(nullptr), HoverText(hoverText), Enabled(enabled) {}
            BigButton( const std::string & name, TogglableNode * prop ) : Name(TrimTogglable(name)), PropVar(nullptr), PropNode(prop), Enabled(true) {}
            BigButton( const std::string & name, std::vector<std::string>* propOptions, int* propOptionIndex, const std::string& hoverText, const std::function<std::string(std::string)> & getItemName) : Name(name), PropOptions(propOptions), PropOptionIndex(propOptionIndex), HoverText(hoverText), Enabled(true), GetItemName(getItemName) { assert(PropOptions->size()>0); }
            bool                IsSelected() const            { return (PropOptions != nullptr)?(true):((PropVar != nullptr)?(*PropVar):(PropNode->IsSelected())); }
            void                SetSelected( bool selected )  { if( PropVar != nullptr ) *PropVar = selected; else if (PropNode != nullptr ) PropNode->SetSelected(selected); else *PropOptionIndex = ( ((*PropOptionIndex)+1) % PropOptions->size() ); }
            std::string         GetText() const 
            {
                if (PropOptions != nullptr)
                {
                    if (GetItemName!=nullptr)
                        return Name + (((*PropOptionIndex) >= 0) ? (GetItemName((*PropOptions)[*PropOptionIndex])) : (std::string("other")));
                    else
                        return Name + (((*PropOptionIndex)>=0)?((*PropOptions)[*PropOptionIndex]):(std::string("other")));
                }
                else
                    return Name;
            }

        };
        std::vector<BigButton> buttons;
        buttons.push_back(BigButton("Animations", m_ui.EnableAnimations, "Animations are not available in reference mode", m_ui.RealtimeMode));
        buttons.push_back(BigButton("AutoExposure", m_ui.ToneMappingParams.autoExposure ) );
        buttons.push_back(BigButton("Sky: ", &envOptions, &envOptionsCurrentIndex, "For more options see Scene/Environment in the main UI", std::function<std::string(std::string)>(TrimSkyDisplayName) ));
        if (materialVariants.size()>0) buttons.push_back(BigButton("Variant: ", &materialVariants, &m_ui.MaterialVariantIndex, "Material or other scene variants", nullptr));
        for (int i = 0; m_ui.TogglableNodes != nullptr && i < m_ui.TogglableNodes->size(); i++)
            buttons.push_back(BigButton((*m_ui.TogglableNodes)[i].UIName, &(*m_ui.TogglableNodes)[i]));

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
                    
                    UI_SCOPED_DISABLE(!buttons[i].Enabled);

                    bool selected = buttons[i].IsSelected();

                    ImGui::PushID(i);
                    float h = 0.33f; 
                    float b = selected ? 1.0f : 0.1f;
                    ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(h, 0.6f * b, 0.6f * b));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(h, 0.7f * b, 0.7f * b));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(h, 0.8f * b, 0.8f * b));
                    if (ImGui::Button(buttons[i].GetText().c_str(), ImVec2(buttonWidth, texSizeA.y * 2)))
                    {
                        buttons[i].SetSelected(!selected);
                        m_ui.ResetAccumulation = true;
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::PopID();

                    if (buttons[i].HoverText.has_value())
                    {
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                            ImGui::SetTooltip("%s", buttons[i].HoverText.value().c_str());
                    }
                }
            }
            ImGui::End();
        }

        if (envOptionsCurrentIndex >= 0 && envOptionsCurrentIndex < envOptions.size() && envOptions[envOptionsCurrentIndex] != envMapOverrideSource )
        {
            m_sceneEditor.SetEnvMapOverrideSource(envOptions[envOptionsCurrentIndex]);
        }

        if (m_ui.MaterialVariantIndex != materialVariantIndexPrev)
        {
            if (FindSubStringIgnoreCase(m_sceneEditor.GetCurrentSceneName(), "bistro") != std::string::npos)
            {
                // bistro dry-wet test
                std::vector<std::string> pavementList = { "LMBR0000163Cobbl_a1d987f5", "LMBR000016bCobbl_8652c51e", "LMBR0000162Paris_c30c71f1", "LMBR0000162Paris_c30c71f1", "LMBR000016cCobbl_f202ecfa", "LMBR0000161Pavem_e2e87964", "LMBR0000168Cobbl_a5a7f4b4", "LMBR0000160Pavem_613287fe", "LMBR000016aCobbl_e1c68d26" };
                for (std::string& id : pavementList)
                    if (auto m = m_sceneEditor.GetLightingPasses().materials()->FindByUniqueID(id))
                    {
                        if (m_ui.MaterialVariantIndex == 0) // reset to default
                            m_sceneEditor.GetLightingPasses().materials()->LoadSingle(*m);
                        else
                        {   // make wet-looking
                            m->Roughness = 0.0f;
                            //m->SpecularColor = float3(0.08f, 0.08f, 0.08f);
                        }
                        m->GPUDataDirty = true;
                    }

                std::vector<std::string> emissivesList = { "LMBR0000172Paris_1d83765c" /*bollards*/, "LMBR00000aeGreen_04f5ae02" /*green leaves*/, "LMBR00000afOrang_a907f305" /*yellow leaves*/, "LMBR00000b0Branc_5990161e" /*branches*/ };
                for (std::string& id : emissivesList)
                    if (auto m = m_sceneEditor.GetLightingPasses().materials()->FindByUniqueID(id))
                    {
                        if (m_ui.MaterialVariantIndex == 0 || m_ui.MaterialVariantIndex == 1) // reset to default
                            m_sceneEditor.GetLightingPasses().materials()->LoadSingle(*m);
                        else
                        {   // silly stuff
                            if (id == "LMBR0000172Paris_1d83765c")
                            {
                                m->EmissiveColor = float3( 0.01f, 1.0f, 0.1f );
                                m->EmissiveIntensity = 0.5f;
                            }
                            if (id == "LMBR00000aeGreen_04f5ae02" || id == "LMBR00000afOrang_a907f305")
                            {
                                m->EmissiveColor = (id == "LMBR00000aeGreen_04f5ae02")?float3(0.9f, 0.3f, 0.01f):float3(0.001f, 1.0f, 0.01f);
                                m->EmissiveIntensity = 0.6f;
                            }
                            if (id == "LMBR00000b0Branc_5990161e")
                            {
                                m->EmissiveColor = float3(1.0f, 0.001f, 0.005f);
                                m->EmissiveIntensity = 1.0f;
                            }
                        }
                        m->GPUDataDirty = true;
                    }

                if (m_ui.MaterialVariantIndex != 1 && materialVariantIndexPrev != 0) // this one doesn't change emissives so no TLAS/BLAS update needed
                    m_ui.Invalidation.ShaderAndACRefreshDelayedRequest = 0.01f;
            }
        }
        
    }


}

void EditorUI::BuildHierarchyPanel(const PanelLayout& layout)
{
    {
        ImGui::SetNextWindowPos(ImVec2(20.f + layout.defWindowWidth, 10.f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(layout.defWindowWidth, layout.scaledHeight * 0.45f), ImGuiCond_Appearing);
        RAII_SCOPE(ImGui::Begin("Hierarchy", nullptr, ImGuiWindowFlags_None);, ImGui::End(););

        auto scene = m_sceneEditor.GetScene();
        auto* ew = scene ? scene->GetEntityWorld() : nullptr;

        if (ew && ew->root() != ecs::NullEntity)
        {
            bool deleteSelectedEntity = false;
            ImGui::Text("Objects: %zu mesh, %u 3DGS", scene->GetMeshInstances().size(), m_ui.GaussianSplats.ObjectCount);
            ImGui::Separator();

            if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                BuildHierarchyNodeUI(m_ui, *scene, ew->root());
                ImGui::TreePop();
            }

            const bool hierarchyFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const auto* parentComp = (m_ui.SelectedEntity != ecs::NullEntity)
                ? ew->world().tryGet<caustica::scene::ParentComponent>(m_ui.SelectedEntity)
                : nullptr;
            const bool canDeleteSelected = m_ui.SelectedEntity != ecs::NullEntity
                && parentComp != nullptr
                && parentComp->parent != ecs::NullEntity;
            if (canDeleteSelected && hierarchyFocused && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
                deleteSelectedEntity = true;

            if (deleteSelectedEntity)
            {
                m_sceneEditor.DeleteSceneNode(m_ui.SelectedEntity);
            }
        }
        else
        {
            ImGui::TextDisabled("No scene loaded.");
        }
    }


}


} // namespace caustica::editor

