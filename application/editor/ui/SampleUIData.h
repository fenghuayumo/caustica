#pragma once

#include <core/command_line.h>
#include <render/Core/PathTracerSettings.h>

#include "EditorUIState.h"

namespace caustica::editor
{

struct SampleUIData : PathTracerSettings, EditorUIState
{
};

void InitializeSampleUIDataFromCommandLine(SampleUIData& ui, const CommandLineOptions& cmdLine);

} // namespace caustica::editor
