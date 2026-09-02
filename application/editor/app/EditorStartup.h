#pragma once

#include <engine/EngineApp.h>

#include "EditorCommandLine.h"

#include <string>

namespace caustica::editor
{

// Parses engine and editor command lines into a single EngineAppDesc.
bool ProcessEditorStartupCommandLine(int argc, char const* const* argv,
    EngineAppDesc& desc,
    EditorCommandLine& editorCmdLine,
    std::string& preferredScene);

} // namespace caustica::editor
