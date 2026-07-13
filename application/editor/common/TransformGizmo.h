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

} // namespace caustica::editor
