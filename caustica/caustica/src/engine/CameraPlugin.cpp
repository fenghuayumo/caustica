#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneSessionSystems.h>
#include <engine/SceneViewState.h>

#include <render/worldRenderer/WorldRenderer.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>

#include <algorithm>

namespace caustica::sceneSession
{

void updateCamera(App& app, float elapsedTimeSeconds)
{
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    GpuRenderSubsystem* gpuRenderSubsystem = gpuRender(app);
    if (!cfg || !vs || !gpuRenderSubsystem)
        return;

    CameraController& cam = gpuRenderSubsystem->camera();
    cam.camera().setMoveSpeed(cfg->CameraMoveSpeed);

    cam.selectedCameraIndex() = std::min(cam.selectedCameraIndex(), sceneCameraCount(app) - 1);
    if (cam.selectedCameraIndex() > 0)
    {
        const std::shared_ptr<Scene> activeScene = scene(app);
        if (activeScene)
        {
            const auto& cameraEntities = activeScene->getCameraEntities();
            const uint32_t camIdx = cam.selectedCameraIndex() - 1;
            const auto* ew = (camIdx < cameraEntities.size()) ? activeScene->getEntityWorld() : nullptr;
            if (ew)
            {
                ecs::Entity camEntity = cameraEntities[camIdx];
                const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
                const auto* persData = camComp ? scene::tryGetPerspectiveCameraData(*camComp) : nullptr;
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                if (persData && globalComp)
                    vs->cameraController.syncFromSceneCamera(*persData, globalComp->transform);
            }
        }
    }

    cam.camera().animate(elapsedTimeSeconds);

    if (auto* wr = worldRenderer(app))
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
        if (auto* wr = worldRenderer(app))
            wr->setGaussianSplatTemporalReset(true);
    }
}

void registerCameraPlugin(App& app)
{
    app.addSystemAfter(AppSchedule::update, "SceneSession.updateCamera", "SceneSession.animate", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        sceneSession::updateCamera(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica::sceneSession
