#pragma once

#include <math/math.h>
#include <scene/Scene.h>
#include <string>
#include <vector>
#include <memory>

using namespace caustica::math;

struct TogglableNode
{
    caustica::SceneGraphNode * SceneNode;
    dm::double3                     OriginalTranslation;
    std::string                     UIName;
    bool                            IsSelected() const;
    void                            SetSelected( bool selected ) ;
};

struct EditorUIState
{

    bool                                ShowUI                                  = true;
    bool                                RenderWhenOutOfFocus                    = false; // if window is out of focus window render loop is paused
    bool                                ShowConsole                             = false;
    std::shared_ptr<caustica::Material> SelectedMaterial;
    std::shared_ptr<caustica::SceneGraphNode> SelectedNode;
    std::weak_ptr<caustica::SceneGraphNode> InspectorRotationNode;
    dm::dquat                           InspectorRotationQuat                   = dm::dquat::identity();
    dm::float3                          InspectorRotationEulerDeg               = dm::float3(0.0f);
    bool                                InspectorRotationEulerValid             = false;
    bool                                ShaderReloadRequested                   = false;
    bool                                AccelerationStructRebuildRequested      = false;
    float                               ShaderAndACRefreshDelayedRequest        = 0.0f;
    bool                                ExperimentalPhotoModeScreenshot         = false;

    bool                                ShowSceneTweakerWindow = false;
    
    bool                                ShowDeltaTree = false;
    bool                                ShowMaterialEditor = true;  // this makes material editor default right click option
    bool                                ShowInspector = true;       // combined Material + Transform inspector panel

    std::shared_ptr<std::vector<TogglableNode>> TogglableNodes = nullptr;
    uint32_t                            GaussianSplatCount = 0;
    uint32_t                            GaussianSplatObjectCount = 0;
    std::string                         GaussianSplatFileName;
    bool                                SelectedGaussianSplat = false;

    std::vector<std::string>            PendingDroppedFiles;                            // files dropped via drag-and-drop, consumed by Sample each frame
};

