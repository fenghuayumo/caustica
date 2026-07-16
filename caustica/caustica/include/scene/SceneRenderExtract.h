#pragma once

#include <cstdint>
#include <ecs/Entity.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderData.h>

struct PathTracerSettings;

namespace caustica
{
class CameraController;
}

namespace caustica::scene
{
    class SceneEntityWorld;
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

    // Logic-thread only. Copies settings / runtime into the snapshot, then resolves
    // ActiveCameraRenderProxy from free CameraController or CameraRenderProxy.
    // Consumes one-shot flags on the live settings object after copy.
    void extractSessionRenderState(const SessionRenderExtractInputs& inputs, SceneRenderData& out);

    // Logic-thread helpers shared by Update (UI preview) and Extract (RT snapshot).
    [[nodiscard]] CameraRenderProxy makeCameraRenderProxy(
        ecs::Entity entity,
        const CameraComponent& component,
        const GlobalTransformComponent& global);

    void applyCameraRenderProxyToController(
        const CameraRenderProxy& proxy,
        CameraController& camera,
        PathTracerSettings* settings = nullptr);

} // namespace caustica::scene
