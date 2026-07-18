#pragma once

#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>

#include "EditorHost.h"

#include <memory>

namespace caustica::editor
{

// Shared graphics/bootstrap via EngineApp; EditorPlugin is a pure delta.
[[nodiscard]] std::unique_ptr<caustica::EngineApp> createEditorEngine(
    EditorHost& host,
    int argc,
    const char* const* argv,
    caustica::AppHook preGpuDeviceInit = nullptr);

} // namespace caustica::editor
