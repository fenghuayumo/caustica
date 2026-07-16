#pragma once

#include "SceneEditor.h"

#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>

#include <cassert>

namespace caustica::editor
{

// Stable access into App / GpuRender for editor code. Prefer these over SceneEditor
// forwarding helpers (scene/camera/env/feedback/...), which are being removed.

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
