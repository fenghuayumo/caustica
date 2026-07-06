#pragma once

#include <ecs/Entity.h>
#include <engine/SceneViewState.h>

#include "ui/EditorUIData.h"

#include <memory>
#include <string>

namespace caustica::editor
{

class CaptureScriptManager;

struct EditorState
{
    std::string loadedSceneName;
};

struct CaptureScriptState
{
    CaptureScriptManager* manager = nullptr;
};

struct SelectionState
{
    explicit SelectionState(EditorUIState& editorUi)
        : editor(&editorUi)
    {
    }

    EditorUIState* editor = nullptr;

    [[nodiscard]] ecs::Entity selectedEntity() const
    {
        return editor ? editor->SelectedEntity : ecs::NullEntity;
    }
};

struct EditorCameraState
{
    explicit EditorCameraState(SceneViewState& sceneViewState)
        : viewState(&sceneViewState)
    {
    }

    SceneViewState* viewState = nullptr;
};

using EditorUiData = EditorUIData;

} // namespace caustica::editor
