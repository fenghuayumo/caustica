#pragma once

#include <render/Core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

struct CommandLineOptions;

namespace caustica::render
{

// Host-owned renderer settings and per-frame runtime state (picking, invalidation, splat summary).
struct RenderSessionState : PathTracerSettings, RenderRuntimeState
{
};

void ApplyPerformancePreset(PathTracerSettings& settings, const PerformancePreset& preset);
bool MatchesPerformancePreset(const PathTracerSettings& settings, const PerformancePreset& preset);

void InitializeRenderSessionStateFromCommandLine(RenderSessionState& state, const CommandLineOptions& cmdLine);

} // namespace caustica::render
