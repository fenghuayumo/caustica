/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

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
