#include <engine/Input.h>
#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/AppResources.h>
#include <engine/RenderSessionApi.h>
#include <engine/ResolvedActiveCamera.h>
#include <render/core/CameraController.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>
#include <backend/GpuDevice.h>
#include <backend/GpuSurface.h>
#include <render/core/PathTracerSettings.h>
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
    auto* cam = app.tryResource<CameraController>();
    if (!cfg || !cam)
        return;

    cam->camera().setMoveSpeed(cfg->CameraMoveSpeed);

    const std::shared_ptr<Scene> scene = activeScene(app);
    const auto* ew = scene ? scene->getEntityWorld() : nullptr;
    const auto* cameraEntities = ew ? &ew->cameraEntitiesInRegistrationOrder() : nullptr;
    const uint cameraCount = cameraEntities
        ? static_cast<uint>(cameraEntities->size()) + 1
        : 1;
    cam->selectedCameraIndex() = std::min(cam->selectedCameraIndex(), cameraCount - 1);

    // Logic-side preview of the selected scene camera (same proxy math as ResolveActiveCamera).
    if (cam->selectedCameraIndex() > 0 && cameraEntities)
    {
        const uint32_t camIdx = cam->selectedCameraIndex() - 1;
        if (camIdx < cameraEntities->size())
        {
            const ecs::Entity camEntity = (*cameraEntities)[camIdx];
            const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
            const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
            if (camComp && globalComp)
            {
                const scene::CameraRenderProxy proxy =
                    scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                scene::applyCameraRenderProxyToController(proxy, *cam, cfg);
            }
        }
    }

    cam->camera().animate(elapsedTimeSeconds);

    if (cfg->CameraAntiRRSleepJitter > 0)
    {
        float off = 0.05f * ((renderFrameIndex(app) % 2)
            ? (-cfg->CameraAntiRRSleepJitter)
            : cfg->CameraAntiRRSleepJitter);

        math::float3 dir = cam->camera().getDir();
        math::float3 right = math::normalize(math::cross(dir, cam->camera().getUp()));
        math::affine3 rot = math::rotation(right, off);
        dir = rot.transformVector(dir);

        cam->camera().lookTo(cam->camera().getPosition(), dir, cam->camera().getUp());
    }

    // Must run before updateViews: default CameraUpdateParams::frameIndex == 0 makes
    // updateViews() call syncPreviousViewFromCurrent() → updateLastCameraState(), which
    // would hide movement and skip PathTracer ResetAccumulation (ghosted accumulation).
    if (cam->cameraMovedSinceLastFrame())
    {
        cam->updateLastCameraState();
        if (!cfg->RealtimeMode)
            cfg->ResetAccumulation = true;
        setGaussianSplatTemporalReset(app, true);
    }

    // Logic-thread CameraController PlanarView is logic-side only (gizmo / 3DGS CPU pick).
    if (GpuSurface* gpuSurface = app.getSurface(); gpuSurface && app.getGpuDevice() && !app.getGpuDevice()->isHeadless())
    {
        int width = 0;
        int height = 0;
        gpuSurface->getWindowDimensions(width, height);
        if (width > 0 && height > 0)
        {
            CameraUpdateParams viewParams;
            viewParams.renderSize = math::uint2{ uint(width), uint(height) };
            viewParams.displayAspectRatio = float(width) / float(height);
            // Non-zero so updateViews does not treat this as "frame 0" and rewrite last pose.
            viewParams.frameIndex = 1;
            cam->updateViews(viewParams);
        }
    }
}

void resolveActiveCamera(App& app)
{
    auto* cam = app.tryResource<CameraController>();
    auto* resolved = app.tryResource<ResolvedActiveCamera>();
    if (!cam || !resolved)
        return;

    const std::shared_ptr<Scene> scene = activeScene(app);
    scene::SceneEntityWorld* ew = scene ? scene->getEntityWorld() : nullptr;

    // selectedIndex 0 = free camera; 1..N = scene cameras in registration order.
    // Runs after TransformPropagate so GlobalTransform is current for RT.
    if (cam->selectedCameraIndex() > 0 && ew)
    {
        const auto& cameraEntities = ew->cameraEntitiesInRegistrationOrder();
        const uint32_t camIdx = cam->selectedCameraIndex() - 1;
        if (camIdx < cameraEntities.size())
        {
            const ecs::Entity camEntity = cameraEntities[camIdx];
            const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
            const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
            if (camComp && globalComp)
            {
                const scene::CameraRenderProxy proxy =
                    scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                if (proxy.projection == scene::CameraProjectionKind::Perspective)
                {
                    scene::fillActiveCameraFromPerspectiveProxy(
                        proxy, cam->selectedCameraIndex(), resolved->camera);
                    return;
                }
            }
        }
    }

    scene::fillActiveCameraFromFreeController(*cam, resolved->camera);
}

void CameraPlugin::configureSchedules(App& app)
{
    app.addSystem<system_label::SceneUpdateCamera>(AppSchedule::update, [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;
        applyCameraWindowInput(ctx.app);
        updateCamera(ctx.app, ctx.deltaTimeSeconds);
    });

    app.addSystem<system_label::SceneResolveActiveCamera>(
        AppSchedule::PostUpdate,
        [](SystemContext& ctx) { resolveActiveCamera(ctx.app); },
        AppSystemOrdering{}
            .inSet<system_set::TransformPropagate>()
            .runAfter<system_label::SceneRefreshEntityWorld>());
}

} // namespace caustica
