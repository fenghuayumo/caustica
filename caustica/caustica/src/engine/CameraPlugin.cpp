#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <scene/SceneRenderExtract.h>

#include <algorithm>

namespace caustica
{

void updateCamera(App& app, float elapsedTimeSeconds)
{
    auto* cfg = app.tryResource<PathTracerSettings>();
    auto* gr = app.tryResource<GpuRenderSubsystem>();
    if (!cfg || !gr)
        return;

    CameraController& cam = gr->camera();
    cam.camera().setMoveSpeed(cfg->CameraMoveSpeed);

    const std::shared_ptr<Scene> activeScene =
        gr->sceneManager() ? gr->sceneManager()->getScene() : nullptr;
    const uint cameraCount = activeScene
        ? static_cast<uint>(activeScene->getCameraEntities().size()) + 1
        : 1;
    cam.selectedCameraIndex() = std::min(cam.selectedCameraIndex(), cameraCount - 1);

    // Logic-side preview of the selected scene camera (same proxy math Extract uses).
    // RT still consumes ActiveCameraRenderProxy from the published snapshot.
    if (cam.selectedCameraIndex() > 0 && activeScene)
    {
        const auto& cameraEntities = activeScene->getCameraEntities();
        const uint32_t camIdx = cam.selectedCameraIndex() - 1;
        const auto* ew = (camIdx < cameraEntities.size()) ? activeScene->getEntityWorld() : nullptr;
        if (ew)
        {
            const ecs::Entity camEntity = cameraEntities[camIdx];
            const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
            const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
            if (camComp && globalComp)
            {
                const scene::CameraRenderProxy proxy =
                    scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                scene::applyCameraRenderProxyToController(proxy, cam, cfg);
            }
        }
    }

    cam.camera().animate(elapsedTimeSeconds);

    if (auto* wr = gr->worldRenderer())
    {
        if (cfg->CameraAntiRRSleepJitter > 0)
        {
            float off = 0.05f * ((wr->getFrameIndex() % 2)
                ? (-cfg->CameraAntiRRSleepJitter)
                : cfg->CameraAntiRRSleepJitter);

            math::float3 dir = cam.camera().getDir();
            math::float3 right = math::normalize(math::cross(dir, cam.camera().getUp()));
            math::affine3 rot = math::rotation(right, off);
            dir = rot.transformVector(dir);

            cam.camera().lookTo(cam.camera().getPosition(), dir, cam.camera().getUp());
        }
    }

    if (cam.cameraMovedSinceLastFrame())
    {
        cam.updateLastCameraState();
        if (!cfg->RealtimeMode)
            cfg->ResetAccumulation = true;
        if (auto* wr = gr->worldRenderer())
            wr->setGaussianSplatTemporalReset(true);
    }
}

void registerCameraPlugin(App& app)
{
    app.addSystem(AppSchedule::update, "Scene.updateCamera", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;
        updateCamera(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica
