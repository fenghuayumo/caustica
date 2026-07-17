#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SessionCamera.h>
#include <engine/SceneQuery.h>
#include <render/core/CameraController.h>
#include <render/worldRenderer/WorldRenderer.h>
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

[[nodiscard]] inline render::WorldRenderer* editorWorldRenderer(SceneEditor& editor)
{
    return editor.app() ? caustica::worldRenderer(*editor.app()) : nullptr;
}

[[nodiscard]] inline render::WorldRenderer* editorWorldRenderer(const SceneEditor& editor)
{
    return editor.app() ? caustica::worldRenderer(*editor.app()) : nullptr;
}

[[nodiscard]] inline render::WorldRenderer& requireWorldRenderer(SceneEditor& editor)
{
    render::WorldRenderer* wr = editorWorldRenderer(editor);
    assert(wr);
    return *wr;
}

[[nodiscard]] inline GpuSharedCaches* editorGpuSharedCaches(SceneEditor& editor)
{
    return editor.app() ? caustica::gpuSharedCaches(*editor.app()) : nullptr;
}

[[nodiscard]] inline const GpuSharedCaches* editorGpuSharedCaches(const SceneEditor& editor)
{
    return editor.app() ? caustica::gpuSharedCaches(*editor.app()) : nullptr;
}

} // namespace caustica::editor
