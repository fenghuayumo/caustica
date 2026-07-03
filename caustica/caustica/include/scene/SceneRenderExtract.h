#pragma once

namespace caustica::scene
{
    class SceneEntityWorld;
    class SceneRenderData;

    void ExtractSceneRenderData(const SceneEntityWorld& entityWorld, SceneRenderData& out);

} // namespace caustica::scene
