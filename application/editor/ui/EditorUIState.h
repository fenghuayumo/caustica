#pragma once

#include <math/math.h>
#include <scene/Scene.h>

#include <memory>
#include <string>
#include <vector>

namespace caustica::editor
{

struct TogglableNode
{
    caustica::ecs::Entity Entity = caustica::ecs::NullEntity;
    caustica::scene::SceneEntityWorld* EntityWorld = nullptr;
    dm::double3 OriginalTranslation;
    std::string UIName;

    bool IsSelected() const;
    void SetSelected(bool selected);
};

struct EditorWindowState
{
    bool ShowUI = true;
    bool RenderWhenOutOfFocus = false;
    bool ShowConsole = false;
    bool ShowSceneTweakerWindow = false;
    bool ShowDeltaTree = false;
    bool ShowMaterialEditor = true;
    bool ShowInspector = true;
};

struct EditorSelectionState
{
    std::shared_ptr<caustica::Material> SelectedMaterial;
    caustica::ecs::Entity SelectedEntity = caustica::ecs::NullEntity;
    // Queued on the UI/render thread; applied on the main thread before scene animate.
    caustica::ecs::Entity PendingDeleteEntity = caustica::ecs::NullEntity;
    caustica::ecs::Entity InspectorRotationEntity = caustica::ecs::NullEntity;
    dm::dquat InspectorRotationQuat = dm::dquat::identity();
    dm::float3 InspectorRotationEulerDeg = dm::float3(0.0f);
    bool InspectorRotationEulerValid = false;
    bool SelectedGaussianSplat = false;
    std::shared_ptr<std::vector<TogglableNode>> TogglableNodes = nullptr;

    // ImGuizmo transform gizmo state (operation/mode values match ImGuizmo enums)
    bool ShowTransformGizmo = true;
    bool GizmoEnabled = true;
    int GizmoOperation = 7; // ImGuizmo::TRANSLATE
    int GizmoMode = 0;      // ImGuizmo::LOCAL
    bool GizmoSnapEnabled = false;
    float GizmoSnapTranslation[3] = { 1.f, 1.f, 1.f };
    float GizmoSnapRotation = 15.f;
    float GizmoSnapScale = 0.1f;
    // Updated each UI frame; input router uses this to avoid stealing gizmo clicks.
    bool GizmoCapturingInput = false;
};

struct EditorFileDropState
{
    std::vector<std::string> PendingDroppedFiles;
};

struct EditorUIState : EditorWindowState, EditorSelectionState, EditorFileDropState
{
    bool ExperimentalPhotoModeScreenshot = false;
};

} // namespace caustica::editor
