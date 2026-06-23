#pragma once

namespace caustica
{

// Forward declarations
class CommandLine;

// Low-level system initialization and shutdown.
// Must be called before any other engine subsystems.
namespace coresystem
{

// Initialize all core subsystems: log, memory, job system, command line, file system.
// Returns true on success.
bool init(int argc, char** argv);

// Shut down all core subsystems in reverse order.
void shutdown();

// Get the parsed command line
CommandLine* getCommandLine();

} // namespace coresystem
} // namespace caustica
