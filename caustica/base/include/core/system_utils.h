#pragma once

#include <string>
#include <tuple>

namespace caustica
{

// --- System shell ---

// Execute a shell command. Returns (exitCode, stdout, stderr).
// On Windows, optionally uses cmd /C wrapper (useCmd=true).
// Currently only supports blockOnExecution=true.
std::tuple<int, std::string, std::string> SystemShell(
    const std::string& command,
    bool useCmd = false,
    bool blockOnExecution = true);

} // namespace caustica
