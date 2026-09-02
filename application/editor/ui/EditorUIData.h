#pragma once

#include <core/command_line.h>
#include <render/RenderAppState.h>

#include "EditorUIState.h"

namespace caustica::editor
{

struct EditorUIData
{
    caustica::render::RenderAppState& render;
    EditorUIState editor;

    explicit EditorUIData(caustica::render::RenderAppState& renderState)
        : render(renderState)
    {
    }
};

void InitializeEditorUIDataFromCommandLine(EditorUIData& ui, const CommandLineOptions& cmdLine);

} // namespace caustica::editor
