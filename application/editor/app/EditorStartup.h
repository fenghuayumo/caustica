#pragma once

#include <backend/GpuDevice.h>
#include <core/command_line.h>

#include <string>

namespace caustica::editor
{

// Parses editor command line, applies host/console defaults, and fills GpuDeviceCreateDesc.
bool ProcessEditorStartupCommandLine(int argc, char const* const* argv,
    CommandLineOptions& cmdLine,
    caustica::GpuDeviceCreateDesc& createDesc,
    std::string& preferredScene);

} // namespace caustica::editor
