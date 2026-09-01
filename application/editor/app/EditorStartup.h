#pragma once

#include <backend/GpuDevice.h>
#include <core/command_line.h>

#include "EditorCommandLine.h"

#include <string>

namespace caustica::editor
{

// Parses engine and editor command lines, applies host/console defaults, and fills GpuDeviceCreateDesc.
bool ProcessEditorStartupCommandLine(int argc, char const* const* argv,
    CommandLineOptions& cmdLine,
    EditorCommandLine& editorCmdLine,
    caustica::GpuDeviceCreateDesc& createDesc,
    std::string& preferredScene);

} // namespace caustica::editor
