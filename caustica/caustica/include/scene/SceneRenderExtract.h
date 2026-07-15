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

    // What changed since the last extract into the logic-side cache.
    // Structure → rebuild proxy lists; transforms → patch fields; neither → skinned only.
    struct SceneRenderExtractFlags
    {
        bool structureChanged = true;
        bool transformsChanged = true;
    };

    // Logic-thread only. Updates `inout` in place (UE SceneProxy sync / Bevy Extract).
    // Callers keep a persistent cache and copy it into the triple-buffer publish slot.
    void extractSceneRenderData(
        SceneEntityWorld& entityWorld,
        SceneRenderData& inout,
        uint32_t frameIndex,
        SceneRenderExtractFlags flags = {});

    // Logic-thread only. Copies camera / settings into the same snapshot slot (no ECS).
    // Consumes one-shot flags on the live settings object after copy.
    void extractSessionRenderState(const SessionRenderExtractInputs& inputs, SceneRenderData& out);

} // namespace caustica::scene
