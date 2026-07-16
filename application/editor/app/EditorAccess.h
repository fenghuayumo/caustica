#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneApi.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>

#include <cassert>
#include <memory>

namespace caustica::editor
{

// Stable access into App / scene / GPU for editor code.
// Prefer editorScene / editorEntityWorld over digging through GpuRenderSubsystem.

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

[[nodiscard]] inline std::shared_ptr<Scene> editorScene(SceneEditor& editor)
{
    return editor.app() ? caustica::activeScene(*editor.app()) : nullptr;
}

[[nodiscard]] inline std::shared_ptr<Scene> editorScene(const SceneEditor& editor)
{
    return editor.app() ? caustica::activeScene(*editor.app()) : nullptr;
}

[[nodiscard]] inline scene::SceneEntityWorld* editorEntityWorld(SceneEditor& editor)
{
    return editor.app() ? caustica::entityWorld(*editor.app()) : nullptr;
}

[[nodiscard]] inline scene::SceneEntityWorld* editorEntityWorld(const SceneEditor& editor)
{
    return editor.app() ? caustica::entityWorld(*editor.app()) : nullptr;
}

[[nodiscard]] inline ::SceneManager* editorSceneManager(SceneEditor& editor)
{
    return editor.app() ? caustica::sceneManager(*editor.app()) : nullptr;
}

[[nodiscard]] inline const ::SceneManager* editorSceneManager(const SceneEditor& editor)
{
    return editor.app() ? caustica::sceneManager(*editor.app()) : nullptr;
}

[[nodiscard]] inline GpuRenderSubsystem* editorGpu(SceneEditor& editor)
{
    return editor.app() ? editor.app()->tryResource<GpuRenderSubsystem>() : nullptr;
}

[[nodiscard]] inline const GpuRenderSubsystem* editorGpu(const SceneEditor& editor)
{
    return editor.app() ? editor.app()->tryResource<GpuRenderSubsystem>() : nullptr;
}

[[nodiscard]] inline GpuRenderSubsystem& requireGpu(SceneEditor& editor)
{
    GpuRenderSubsystem* gpu = editorGpu(editor);
    assert(gpu);
    return *gpu;
}

} // namespace caustica::editor
