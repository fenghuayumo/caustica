#pragma once

#include <core/command_line.h>
#include <render/RenderSessionState.h>

#include "EditorUIState.h"

namespace caustica::editor
{

struct EditorUIData
{
    caustica::render::RenderSessionState session;
    EditorUIState editor;
};

void InitializeEditorUIDataFromCommandLine(EditorUIData& ui, const CommandLineOptions& cmdLine);

} // namespace caustica::editor
