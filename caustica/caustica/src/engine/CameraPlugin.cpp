#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneViewState.h>

#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>

#include <algorithm>

namespace caustica
{

void updateCamera(App& app, float elapsedTimeSeconds)
{
    auto* cfg = app.tryResource<PathTracerSettings>();
    auto* vs = app.tryResource<SceneViewState>();
    auto* gr = app.tryResource<GpuRenderSubsystem>();
    if (!cfg || !vs || !gr)
        return;

    CameraController& cam = gr->camera();
    cam.camera().setMoveSpeed(cfg->CameraMoveSpeed);

    const uint cameraCount = gr->sceneManager() && gr->sceneManager()->getScene()
        ? static_cast<uint>(gr->sceneManager()->getScene()->getCameraEntities().size()) + 1
        : 1;
    cam.selectedCameraIndex() = std::min(cam.selectedCameraIndex(), cameraCount - 1);
    if (cam.selectedCameraIndex() > 0)
    {
        const std::shared_ptr<Scene> activeScene =
            gr->sceneManager() ? gr->sceneManager()->getScene() : nullptr;
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
