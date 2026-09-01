#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/GpuSharedCaches.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <render/core/CameraController.h>
#include <render/WorldRenderer.h>
#include <scene/Scene.h>

// Editor may use internal WorldRenderer access for remaining pass debug UI,
// picking submit, and material-cache lookup until those surfaces move onto
// public APIs. Frame/session hot paths must use RenderSessionApi / SceneQuery.

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

[[nodiscard]] inline CameraController* editorCameraController(SceneEditor& editor)
{
    return editor.app() ? caustica::cameraController(*editor.app()) : nullptr;
}

[[nodiscard]] inline const CameraController* editorCameraController(const SceneEditor& editor)
{
    return editor.app() ? caustica::cameraController(*editor.app()) : nullptr;
}

[[nodiscard]] inline CameraController* editorCamera(SceneEditor& editor)
{
    return editorCameraController(editor);
}

[[nodiscard]] inline const CameraController* editorCamera(const SceneEditor& editor)
{
    return editorCameraController(editor);
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
