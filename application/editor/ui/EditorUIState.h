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
    caustica::SceneGraphNode* SceneNode = nullptr;
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
    std::shared_ptr<caustica::SceneGraphNode> SelectedNode;
    std::weak_ptr<caustica::SceneGraphNode> InspectorRotationNode;
    dm::dquat InspectorRotationQuat = dm::dquat::identity();
    dm::float3 InspectorRotationEulerDeg = dm::float3(0.0f);
    bool InspectorRotationEulerValid = false;
    bool SelectedGaussianSplat = false;
    std::shared_ptr<std::vector<TogglableNode>> TogglableNodes = nullptr;
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
