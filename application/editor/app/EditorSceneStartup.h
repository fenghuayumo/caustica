#pragma once

namespace caustica
{
class App;
}

namespace caustica::editor
{

class SceneEditor;

struct EditorSceneStartupConfig
{
    SceneEditor* sceneEditor = nullptr;
    bool postAppInit = true;
};

void registerEditorSceneStartup(caustica::App& app, const EditorSceneStartupConfig& config);
void registerEditorUISubsystemLifecycle(caustica::App& app);

} // namespace caustica::editor
