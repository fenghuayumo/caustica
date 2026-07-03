#pragma once

#include <events/Event.h>

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

} // namespace caustica::editor
