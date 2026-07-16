#pragma once

#include <string>

#include <shaders/PathTracer/Config.h>

namespace caustica { class Material; }

namespace caustica::render { struct RenderAppState; }

namespace caustica::editor
{
class EditorUIState;
class SceneEditor;

struct LocalConfig
{
    static void        PreferredSceneOverride( std::string & preferredScene );
    static void        PostAppInit(caustica::render::RenderAppState & renderState );
    static void postMaterialLoad(caustica::Material& material);
    static void        PostSceneLoad( SceneEditor & sample, caustica::render::RenderAppState & renderState, EditorUIState & editorState );
};

} // namespace caustica::editor