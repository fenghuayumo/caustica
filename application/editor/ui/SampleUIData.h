#pragma once

#include <core/command_line.h>
#include <render/Core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

#include "EditorUIState.h"

namespace caustica::editor
{

struct SampleUIData : PathTracerSettings, caustica::render::RenderRuntimeState, EditorUIState
{
};

void InitializeSampleUIDataFromCommandLine(SampleUIData& ui, const CommandLineOptions& cmdLine);

} // namespace caustica::editor
