#pragma once

#include <engine/App.h>
#include <engine/SceneStartup.h>

#include "EditorHost.h"

namespace caustica::editor
{

bool startupEditor(caustica::App& app, EditorHost& host, int argc, const char* const* argv);

} // namespace caustica::editor
