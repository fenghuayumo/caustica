#pragma once

#include <ecs/Entity.h>
#include <engine/SceneViewState.h>

#include "ui/EditorUIData.h"

#include <json/json.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

namespace caustica::editor
{

class CaptureScriptManager;

struct EditorState
{
    std::string loadedSceneName;
    // Source scene JSON cached on load for Save Scene (transform patch-back).
    Json::Value sceneDocument;
    std::filesystem::path sceneDocumentPath;
    bool sceneDocumentValid = false;
    bool saveAsRequired = false;
    std::unordered_set<std::string> editedTransformPaths;
};

struct CaptureScriptState
{
    CaptureScriptManager* manager = nullptr;
};

struct SelectionState
{
    SelectionState() = default;
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
    EditorCameraState() = default;
    explicit EditorCameraState(SceneViewState& sceneViewState)
        : viewState(&sceneViewState)
    {
    }

    SceneViewState* viewState = nullptr;
};

using EditorUiData = EditorUIData;

} // namespace caustica::editor
