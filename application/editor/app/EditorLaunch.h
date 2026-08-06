#pragma once

#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>

#include "SceneEditor.h"

#include <memory>

namespace caustica::editor
{

// Shared graphics/bootstrap via EngineApp; EditorPlugin is a pure delta.
[[nodiscard]] std::unique_ptr<caustica::EngineApp> createEditorEngine(
    SceneEditor& editor,
    int argc,
    const char* const* argv,
    caustica::AppHook preGpuDeviceInit = nullptr);

void installEditorLogFilter();

} // namespace caustica::editor
