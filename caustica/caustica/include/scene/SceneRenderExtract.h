#pragma once

#include <cstdint>

namespace caustica::scene
{
    class SceneEntityWorld;
    class SceneRenderData;

    // Logic-thread only. Builds pure render proxies; may clear FORCE skinning flags on ECS.
    void extractSceneRenderData(SceneEntityWorld& entityWorld, SceneRenderData& out, uint32_t frameIndex);

} // namespace caustica::scene
