#pragma once

#include <cstdint>

namespace caustica
{
class CameraController;
}

namespace caustica::scene
{
    class SceneEntityWorld;
    class SceneRenderData;
    struct SessionRenderExtractInputs;

    // Logic-thread only. Builds pure render proxies; may clear FORCE skinning flags on ECS.
    void extractSceneRenderData(SceneEntityWorld& entityWorld, SceneRenderData& out, uint32_t frameIndex);

    // Logic-thread only. Copies camera / settings into the same snapshot slot (no ECS).
    // Consumes one-shot flags on the live settings object after copy.
    void extractSessionRenderState(const SessionRenderExtractInputs& inputs, SceneRenderData& out);

} // namespace caustica::scene
