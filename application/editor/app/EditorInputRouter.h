#pragma once

#include <events/event.h>

namespace caustica
{
class App;
}

namespace caustica::editor
{

class SceneEditor;

// Routes keyboard/mouse events to camera, game scene, zoom tool, and editor toggles.
class EditorInputRouter
{
public:
    void bind(SceneEditor& sceneEditor);

    void onEvent(caustica::Event& event);

private:
    SceneEditor* m_sceneEditor = nullptr;
};

void updateEditorCameraInputGate(SceneEditor& sceneEditor, caustica::App& app);

} // namespace caustica::editor
