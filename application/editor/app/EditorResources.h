#pragma once

#include <ecs/Entity.h>
#include <engine/SceneViewState.h>

#include "ui/EditorUIData.h"

#include <json/json.h>

#include <chrono>
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

// Transient status-bar feedback (e.g. "Saving scene..." -> "Saved scene 'x' (84 ms)").
enum class EditorStatusKind
{
    None = 0,
    Busy,
    Success,
    Error,
};

struct EditorStatusMessage
{
    EditorStatusKind kind = EditorStatusKind::None;
    std::string text;
    std::chrono::steady_clock::time_point setAt{};
    // How long the message stays visible once idle. <= 0 keeps it until replaced
    // (used for the Busy state that is owned by the pending action).
    float lingerSeconds = 0.f;

    [[nodiscard]] bool visible() const
    {
        if (kind == EditorStatusKind::None || text.empty())
            return false;
        if (lingerSeconds <= 0.f)
            return true;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - setAt).count()
            < lingerSeconds;
    }
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
