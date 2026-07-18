#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/AppResources.h>
#include <engine/SessionCamera.h>
#include <engine/SceneQuery.h>
#include <engine/SystemLabels.h>
#include <render/core/PathTracerSettings.h>
#include <render/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>

#include <algorithm>

namespace caustica
{

void updateCamera(App& app, float elapsedTimeSeconds)
{
    auto* cfg = app.tryResource<PathTracerSettings>();
    auto* sessionCam = app.tryResource<SessionCamera>();
    if (!cfg || !sessionCam)
        return;

    CameraController& cam = sessionCam->camera;
    cam.camera().setMoveSpeed(cfg->CameraMoveSpeed);

    const std::shared_ptr<Scene> scene = activeScene(app);
    const auto* ew = scene ? scene->getEntityWorld() : nullptr;
    const auto* cameraEntities = ew ? &ew->cameraEntitiesInRegistrationOrder() : nullptr;
    const uint cameraCount = cameraEntities
        ? static_cast<uint>(cameraEntities->size()) + 1
        : 1;
    cam.selectedCameraIndex() = std::min(cam.selectedCameraIndex(), cameraCount - 1);

    // Logic-side preview of the selected scene camera (same proxy math Extract uses).
    // RT still consumes ActiveCameraRenderProxy from the published snapshot.
    if (cam.selectedCameraIndex() > 0 && cameraEntities)
    {
        const uint32_t camIdx = cam.selectedCameraIndex() - 1;
        if (camIdx < cameraEntities->size())
        {
            const ecs::Entity camEntity = (*cameraEntities)[camIdx];
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

void CameraPlugin::configureSchedules(App& app)
{
    app.addSystem<system_label::SceneUpdateCamera>(AppSchedule::update, [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;
        updateCamera(ctx.app, ctx.deltaTimeSeconds);
    });
}

} // namespace caustica
