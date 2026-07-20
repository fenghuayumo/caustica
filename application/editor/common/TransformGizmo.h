#pragma once

#include <scene/Scene.h>
#include <render/core/PathTracerSettings.h>

#include "ui/EditorUIState.h"

namespace caustica::editor
{

class SceneEditor;

struct TransformGizmoContext
{
    SceneEditor& sceneEditor;
    EditorUIState& editorUI;
    PathTracerSettings& settings;
};

void BuildTransformGizmoToolbar(EditorUIState& editorUI);
bool DrawTransformGizmo(const TransformGizmoContext& ctx);

// True while a gizmo drag session is in progress (including the release frame
// before the undo command is committed).
[[nodiscard]] bool IsTransformGizmoEditing();

// Drop drag/undo session state so the next frame resyncs from ECS.
void ResetTransformGizmoInteraction();

// Revert an in-progress drag to its before snapshot and suppress writeback until
// the mouse is released. Returns true if a drag session was cancelled.
bool CancelTransformGizmoEdit(SceneEditor& sceneEditor);

} // namespace caustica::editor
