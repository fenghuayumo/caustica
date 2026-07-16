#pragma once

#include <engine/SceneStartup.h>

namespace caustica::editor
{

class SceneEditor;

struct EditorSceneStartupConfig
{
    caustica::SceneAppConfig appConfig;
    SceneEditor* sceneEditor = nullptr;
    bool postAppInit = true;
};

void registerEditorSceneStartup(caustica::App& app, const EditorSceneStartupConfig& config);
void registerEditorUISubsystemLifecycle(caustica::App& app);

} // namespace caustica::editor
