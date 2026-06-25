#pragma once

#include <string>

#include <shaders/PathTracer/Config.h>

class SceneEditor;
struct SampleUIData;

namespace caustica
{
    struct Material;
}

struct LocalConfig
{
    static void        PreferredSceneOverride( std::string & preferredScene );
    static void        PostAppInit(SampleUIData & sampleUI );
    static void        PostMaterialLoad( caustica::Material & material );
    static void        PostSceneLoad( SceneEditor & sample, SampleUIData & sampleUI );
};