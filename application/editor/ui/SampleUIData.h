#pragma once

#include <core/command_line.h>
#include <render/RenderSessionState.h>

#include "EditorUIState.h"

namespace caustica::editor
{

struct SampleUIData : caustica::render::RenderSessionState, EditorUIState
{
};

void InitializeSampleUIDataFromCommandLine(SampleUIData& ui, const CommandLineOptions& cmdLine);

} // namespace caustica::editor
