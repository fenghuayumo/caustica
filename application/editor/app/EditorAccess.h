#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/PathTracingRuntime.h>
#include <engine/RenderInfra.h>
#include <engine/SessionCamera.h>
#include <engine/SceneQuery.h>
#include <render/core/CameraController.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>

#include <cassert>
#include <memory>

namespace caustica::editor
{

// Stable access into App / scene / GPU for editor code.
// Prefer these helpers over digging App resources by hand.

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

[[nodiscard]] inline SessionCamera* editorSessionCamera(SceneEditor& editor)
{
    return editor.app() ? caustica::sessionCameraResource(*editor.app()) : nullptr;
}

[[nodiscard]] inline const SessionCamera* editorSessionCamera(const SceneEditor& editor)
{
    return editor.app() ? caustica::sessionCameraResource(*editor.app()) : nullptr;
}

[[nodiscard]] inline CameraController* editorCamera(SceneEditor& editor)
{
    if (SessionCamera* session = editorSessionCamera(editor))
        return &session->camera;
    return nullptr;
}

[[nodiscard]] inline const CameraController* editorCamera(const SceneEditor& editor)
{
    if (const SessionCamera* session = editorSessionCamera(editor))
        return &session->camera;
    return nullptr;
}

[[nodiscard]] inline PathTracingRuntime* editorPathTracing(SceneEditor& editor)
{
    return editor.app() ? caustica::pathTracingRuntime(*editor.app()) : nullptr;
}

[[nodiscard]] inline const PathTracingRuntime* editorPathTracing(const SceneEditor& editor)
{
    return editor.app() ? caustica::pathTracingRuntime(*editor.app()) : nullptr;
}

[[nodiscard]] inline PathTracingRuntime& requirePathTracing(SceneEditor& editor)
{
    PathTracingRuntime* pt = editorPathTracing(editor);
    assert(pt);
    return *pt;
}

[[nodiscard]] inline RenderInfra* editorRenderInfra(SceneEditor& editor)
{
    return editor.app() ? caustica::renderInfra(*editor.app()) : nullptr;
}

[[nodiscard]] inline const RenderInfra* editorRenderInfra(const SceneEditor& editor)
{
    return editor.app() ? caustica::renderInfra(*editor.app()) : nullptr;
}

[[nodiscard]] inline render::WorldRenderer* editorWorldRenderer(SceneEditor& editor)
{
    return editor.app() ? caustica::worldRenderer(*editor.app()) : nullptr;
}

[[nodiscard]] inline render::WorldRenderer* editorWorldRenderer(const SceneEditor& editor)
{
    return editor.app() ? caustica::worldRenderer(*editor.app()) : nullptr;
}

} // namespace caustica::editor
