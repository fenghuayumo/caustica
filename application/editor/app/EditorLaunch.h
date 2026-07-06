#pragma once

#include <engine/App.h>
#include <engine/SceneSessionStartup.h>

#include "EditorSession.h"

namespace caustica::editor
{

bool startupEditor(caustica::App& app, EditorSession& session, int argc, const char* const* argv);

} // namespace caustica::editor
