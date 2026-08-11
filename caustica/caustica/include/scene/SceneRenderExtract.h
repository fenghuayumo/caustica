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
    struct FrameExtractInputs;

    // What changed since the last extract into the logic-side cache.
    // Structure → rebuild proxy lists + mesh/material snapshots;
    // transforms → Changed<> patch of mesh proxies;
    // lights → refresh light values/transforms without rewriting mesh instances;
    // neither → skinned / camera / splat refresh only.
    struct SceneRenderExtractFlags
    {
        bool structureChanged = true;
        bool transformsChanged = true;
        bool lightsChanged = true;
    };

    // Logic-thread only. Updates `inout` in place (UE SceneProxy sync / Bevy Extract).
    // Core mesh/light/camera/skinned only — leaf types use Extract schedule systems.
    void extractSceneRenderData(
        SceneEntityWorld& entityWorld,
        SceneRenderData& inout,
        uint32_t frameIndex,
        SceneRenderExtractFlags flags = {});

    // Extract-schedule helper (GaussianSplat). Writes `out.gaussianSplats` only.
    void extractGaussianSplatProxies(SceneEntityWorld& entityWorld, SceneRenderData& out);

    // Logic-thread only. Pure copy of settings / runtime / pre-resolved active camera.
    // Consumes one-shot flags on the live settings object after copy.
    void extractFrameRenderState(const FrameExtractInputs& inputs, SceneRenderData& out);

    // Logic-thread helpers shared by Update (UI preview) and ResolveActiveCamera.
    [[nodiscard]] CameraRenderProxy makeCameraRenderProxy(
        ecs::Entity entity,
        const CameraComponent& component,
        const GlobalTransformComponent& global);

    void applyCameraRenderProxyToController(
        const CameraRenderProxy& proxy,
        CameraController& camera,
        PathTracerSettings* settings = nullptr);

    void fillActiveCameraFromFreeController(
        const CameraController& camera, ActiveCameraRenderProxy& out);
    void fillActiveCameraFromPerspectiveProxy(
        const CameraRenderProxy& proxy, uint32_t selectedIndex, ActiveCameraRenderProxy& out);

} // namespace caustica::scene
