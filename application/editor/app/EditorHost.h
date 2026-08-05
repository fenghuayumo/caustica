#pragma once

#include <core/command_line.h>
#include <core/log.h>
#include <render/AppDiagnostics.h>

#include "SceneEditor.h"
#include "ui/EditorUIData.h"

#include <memory>

namespace caustica::editor
{

class RenderSettingsConsoleBinding;

// Host-owned editor bag: cmdline, UI data, diagnostics, SceneEditor shell.
struct EditorHost
{
    CommandLineOptions cmdLine;
    EditorUIData editorUiData;
    render::AppDiagnostics diagnostics;
    SceneEditor sceneEditor;
    std::unique_ptr<RenderSettingsConsoleBinding> console;

    EditorHost();
    ~EditorHost();
};

void installEditorLogFilter(EditorHost& host);

} // namespace caustica::editor
