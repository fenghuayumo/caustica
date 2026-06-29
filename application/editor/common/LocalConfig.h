#pragma once

#include <string>

#include <shaders/PathTracer/Config.h>

namespace caustica { class Material; }

namespace caustica::render { struct RenderSessionState; }

namespace caustica::editor
{
class EditorUIState;
class SceneEditor;

struct LocalConfig
{
    static void        PreferredSceneOverride( std::string & preferredScene );
    static void        PostAppInit(caustica::render::RenderSessionState & sessionState );
    static void        PostMaterialLoad( caustica::Material & material );
    static void        PostSceneLoad( SceneEditor & sample, caustica::render::RenderSessionState & sessionState, EditorUIState & editorState );
};

} // namespace caustica::editor