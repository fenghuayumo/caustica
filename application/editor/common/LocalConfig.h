#pragma once

#include <string>

#include <shaders/PathTracer/Config.h>

namespace caustica { class Material; }

namespace caustica::editor
{

class SceneEditor;
struct SampleUIData;

struct LocalConfig
{
    static void        PreferredSceneOverride( std::string & preferredScene );
    static void        PostAppInit(SampleUIData & sampleUI );
    static void        PostMaterialLoad( caustica::Material & material );
    static void        PostSceneLoad( SceneEditor & sample, SampleUIData & sampleUI );
};

} // namespace caustica::editor