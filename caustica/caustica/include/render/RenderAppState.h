#pragma once

#include <render/core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

struct CommandLineOptions;

namespace caustica::render
{

// Host-owned renderer settings and per-frame runtime state (picking, invalidation, splat summary).
struct RenderAppState
{
    PathTracerSettings settings;
    RenderRuntimeState runtime;
};

void applyPerformancePreset(PathTracerSettings& settings, const PerformancePreset& preset);
bool MatchesPerformancePreset(const PathTracerSettings& settings, const PerformancePreset& preset);

void InitializeRenderAppStateFromCommandLine(RenderAppState& state, const CommandLineOptions& cmdLine);

} // namespace caustica::render
