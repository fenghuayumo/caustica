#pragma once

#include <string>
#include <tuple>

namespace caustica
{

// --- System shell ---

// execute a shell command. Returns (exitCode, stdout, stderr).
// On Windows, optionally uses cmd /C wrapper (useCmd=true).
// Currently only supports blockOnExecution=true.
std::tuple<int, std::string, std::string> systemShell(
    const std::string& command,
    bool useCmd = false,
    bool blockOnExecution = true);

} // namespace caustica
