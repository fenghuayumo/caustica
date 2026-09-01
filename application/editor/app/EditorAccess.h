#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/SceneQuery.h>

#include <cassert>

namespace caustica::editor
{

// Stable access into App / live ECS for editor code.
// Prefer these helpers over digging App resources by hand.
// Render session (passes, picking, materials) goes through RenderSessionApi.
// Camera input goes through CameraApi. Do not include engine/internal/*.

[[nodiscard]] inline App& editorApp(SceneEditor& editor)
{
    assert(editor.app());
    return *editor.app();
}

[[nodiscard]] inline const App& editorApp(const SceneEditor& editor)
{
    assert(editor.app());
    return *editor.app();
}

[[nodiscard]] inline scene::SceneEntityWorld* editorEntityWorld(SceneEditor& editor)
{
    return editor.app() ? caustica::entityWorld(*editor.app()) : nullptr;
}

[[nodiscard]] inline scene::SceneEntityWorld* editorEntityWorld(const SceneEditor& editor)
{
    return editor.app() ? caustica::entityWorld(*editor.app()) : nullptr;
}

} // namespace caustica::editor
