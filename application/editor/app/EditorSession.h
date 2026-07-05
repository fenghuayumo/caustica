#pragma once

#include <core/command_line.h>
#include <core/log.h>
#include <render/SessionDiagnostics.h>

#include "SceneEditor.h"
#include "ui/EditorUIData.h"

namespace caustica::editor
{

struct EditorSession
{
    CommandLineOptions cmdLine;
    EditorUIData editorUiData;
    render::SessionDiagnostics sessionDiagnostics;
    SceneEditor sceneEditor;

    EditorSession();
};

void installEditorLogFilter(EditorSession& session);

} // namespace caustica::editor
