#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <render/core/CameraController.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>

#include <cassert>
#include <cmath>
#include <filesystem>

namespace caustica
{

namespace
{

bool IsActiveSceneCamera(const App& app, ecs::Entity entity)
{
    return ecs::isValid(entity) && activeSceneCameraEntity(app) == entity;
}

bool IsPerspectiveSceneCamera(const App& app, ecs::Entity entity)
{
    auto* ew = entityWorld(app);
    if (!ew || !ecs::isValid(entity))
        return false;
    const auto* camera = scene::tryGetCamera(ew->world(), entity);
    return camera && scene::tryGetPerspectiveCameraData(*camera) != nullptr;
}

bool IsValidVerticalFov(float radians)
{
    return std::isfinite(radians) && radians > 0.f && radians < dm::PI_f;
}

bool IsValidIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    return std::isfinite(fx) && std::isfinite(fy)
        && std::isfinite(cx) && std::isfinite(cy)
        && std::isfinite(width) && std::isfinite(height)
        && fx > 0.f && fy > 0.f && width > 0.f && height > 0.f;
}

bool IsValidCameraPose(const math::float3& position, const math::float3& direction, const math::float3& up)
{
    return dm::all(dm::isfinite(position))
        && dm::all(dm::isfinite(direction))
        && dm::all(dm::isfinite(up))
        && dm::length(direction) > 1e-6f;
}

} // namespace

const std::vector<ecs::Entity>& sceneCameraEntities(const App& app)
{
    static const std::vector<ecs::Entity> kEmpty;
    auto* ew = entityWorld(app);
    return ew ? ew->cameraEntitiesInRegistrationOrder() : kEmpty;
}

uint32_t sceneCameraCount(const App& app)
{
    return static_cast<uint32_t>(sceneCameraEntities(app).size()) + 1;
}

uint32_t selectedCameraIndex(const App& app)
{
    auto* camera = cameraController(app);
    return camera ? camera->selectedCameraIndex() : 0;
}

bool setSelectedCameraIndex(App& app, uint32_t index)
{
    auto* camera = cameraController(app);
    if (!camera)
        return false;

    const uint32_t count = sceneCameraCount(app);
    if (count == 0 || index >= count)
        return false;
    if (index > 0)
    {
        const auto& cameras = sceneCameraEntities(app);
        const uint32_t cameraIndex = index - 1;
        if (cameraIndex >= cameras.size() || !IsPerspectiveSceneCamera(app, cameras[cameraIndex]))
            return false;
    }
    if (camera->selectedCameraIndex() == index)
        return true;
    camera->setSelectedCameraIndex(index);
    camera->markCameraChanged();
    return true;
}

ecs::Entity activeSceneCameraEntity(const App& app)
{
    const uint32_t index = selectedCameraIndex(app);
    if (index == 0)
        return ecs::NullEntity;
    const auto& cameras = sceneCameraEntities(app);
    const uint32_t cameraIndex = index - 1;
    if (cameraIndex >= cameras.size())
        return ecs::NullEntity;
    return IsPerspectiveSceneCamera(app, cameras[cameraIndex])
        ? cameras[cameraIndex]
        : ecs::NullEntity;
}

bool activeCameraIsFree(const App& app)
{
    return !ecs::isValid(activeSceneCameraEntity(app));
}

std::string activeCameraPath(const App& app)
{
    const ecs::Entity entity = activeSceneCameraEntity(app);
    auto* ew = entityWorld(app);
    return ew && ecs::isValid(entity) ? ew->getEntityPath(entity).generic_string() : std::string{};
}

std::string activeCameraName(const App& app)
{
    const ecs::Entity entity = activeSceneCameraEntity(app);
    auto* ew = entityWorld(app);
    return ew && ecs::isValid(entity) ? ew->getEntityName(entity) : std::string{};
}

bool setActiveCamera(App& app, ecs::Entity entity)
{
    if (!ecs::isValid(entity))
    {
        return setSelectedCameraIndex(app, 0);
    }

    const auto& cameras = sceneCameraEntities(app);
    for (size_t i = 0; i < cameras.size(); ++i)
    {
        if (cameras[i] == entity)
        {
            if (!IsPerspectiveSceneCamera(app, entity))
                return false;
            return setSelectedCameraIndex(app, static_cast<uint32_t>(i + 1));
        }
    }
    return false;
}

bool setActiveCameraByPath(App& app, const std::string& path)
{
    if (path.empty())
        return false;
    auto* ew = entityWorld(app);
    if (!ew)
        return false;
    const ecs::Entity entity = ew->findEntity(std::filesystem::path(path));
    if (!ecs::isValid(entity) || !IsPerspectiveSceneCamera(app, entity))
        return false;
    return setActiveCamera(app, entity);
}

float cameraVerticalFOV(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->verticalFOV();
}

float cameraZNear(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->zNear();
}

FirstPersonCamera& currentCamera(App& app)
{
    assert(cameraController(app));
    return cameraController(app)->camera();
}

const FirstPersonCamera& currentCamera(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->camera();
}

void markCameraChanged(App& app)
{
    if (auto* camera = cameraController(app))
        camera->markCameraChanged();
}

const std::shared_ptr<ViewInfo>& currentView(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->view();
}

const ViewInfo& view(const App& app)
{
    assert(cameraController(app));
    return *cameraController(app)->view();
}

std::string currentCameraPosDirUp(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->getPosDirUpString();
}

CameraPose currentCameraPose(const App& app)
{
    const ecs::Entity entity = activeSceneCameraEntity(app);
    if (ecs::isValid(entity))
    {
        if (auto* ew = entityWorld(app))
        {
            scene::CameraWorldLookTo look;
            if (scene::tryGetCameraWorldLookTo(*ew, entity, look))
                return { look.position, look.direction, look.up };
        }
    }
    if (auto* camera = cameraController(app))
        return { camera->camera().getPosition(), camera->camera().getDir(), camera->camera().getUp() };
    return {};
}

bool setCurrentCameraPose(App& app, const CameraPose& pose)
{
    if (!IsValidCameraPose(pose.position, pose.direction, pose.up))
        return false;
    return setCurrentCameraPosDirUp(app, pose.position, pose.direction, pose.up);
}

bool setCurrentCameraPosDirUp(App& app, const std::string& val)
{
    auto* camera = cameraController(app);
    if (!camera || !camera->setFromPosDirUpString(val))
        return false;
    return setCurrentCameraPosDirUp(
        app, camera->camera().getPosition(), camera->camera().getDir(), camera->camera().getUp());
}

bool setCurrentCameraPosDirUp(
    App& app,
    const math::float3& pos,
    const math::float3& dir,
    const math::float3& up)
{
    if (!IsValidCameraPose(pos, dir, up))
        return false;
    const ecs::Entity entity = activeSceneCameraEntity(app);
    if (ecs::isValid(entity))
        return setSceneCameraLookTo(app, entity, pos, dir, up);

    auto* camera = cameraController(app);
    if (!camera)
        return false;
    camera->camera().lookTo(pos, dir, up);
    camera->markCameraChanged();
    return true;
}

bool setCameraVerticalFOV(App& app, float cameraFOV)
{
    if (!IsValidVerticalFov(cameraFOV))
        return false;
    const ecs::Entity entity = activeSceneCameraEntity(app);
    if (ecs::isValid(entity))
        return setSceneCameraVerticalFOV(app, entity, cameraFOV);
    if (auto* camera = cameraController(app))
    {
        camera->setVerticalFOVInteractive(cameraFOV);
        return true;
    }
    return false;
}

bool setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height)
{
    if (!IsValidIntrinsics(fx, fy, cx, cy, width, height))
        return false;
    const ecs::Entity entity = activeSceneCameraEntity(app);
    if (ecs::isValid(entity))
        return setSceneCameraIntrinsics(app, entity, fx, fy, cx, cy, width, height);
    if (auto* camera = cameraController(app))
    {
        camera->setIntrinsicsInteractive(fx, fy, cx, cy, width, height);
        return true;
    }
    return false;
}

bool clearCameraIntrinsics(App& app)
{
    const ecs::Entity entity = activeSceneCameraEntity(app);
    if (ecs::isValid(entity))
        return clearSceneCameraIntrinsics(app, entity);
    if (auto* camera = cameraController(app))
    {
        camera->clearIntrinsicsInteractive();
        return true;
    }
    return false;
}

bool setSceneCameraVerticalFOV(App& app, ecs::Entity entity, float radians)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraVerticalFov(*ew, entity, radians))
        return false;
    if (IsActiveSceneCamera(app, entity))
    {
        if (auto* camera = cameraController(app))
            camera->setVerticalFOVInteractive(radians);
    }
    return true;
}

float sceneCameraVerticalFOV(const App& app, ecs::Entity entity)
{
    auto* ew = entityWorld(app);
    return ew ? scene::getCameraVerticalFov(*ew, entity) : 0.f;
}

bool setSceneCameraLookTo(
    App& app,
    ecs::Entity entity,
    const math::float3& pos,
    const math::float3& dir,
    const math::float3& up)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraWorldLookTo(*ew, entity, pos, dir, up))
        return false;
    if (IsActiveSceneCamera(app, entity))
    {
        if (auto* camera = cameraController(app))
        {
            camera->camera().lookTo(pos, dir, up);
            camera->markCameraChanged();
        }
    }
    return true;
}

bool setSceneCameraIntrinsics(
    App& app,
    ecs::Entity entity,
    float fx,
    float fy,
    float cx,
    float cy,
    float width,
    float height)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraIntrinsics(*ew, entity, fx, fy, cx, cy, width, height))
        return false;
    if (IsActiveSceneCamera(app, entity))
    {
        if (auto* camera = cameraController(app))
            camera->setIntrinsicsInteractive(fx, fy, cx, cy, width, height);
    }
    return true;
}

bool clearSceneCameraIntrinsics(App& app, ecs::Entity entity)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::clearCameraIntrinsics(*ew, entity))
        return false;
    if (IsActiveSceneCamera(app, entity))
    {
        if (auto* camera = cameraController(app))
            camera->clearIntrinsicsInteractive();
    }
    return true;
}

bool setSceneCameraZNear(App& app, ecs::Entity entity, float zNear)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraZNear(*ew, entity, zNear))
        return false;
    if (IsActiveSceneCamera(app, entity))
    {
        if (auto* camera = cameraController(app))
        {
            camera->setZNear(zNear);
            camera->markCameraChanged();
        }
    }
    return true;
}

bool setSceneCameraZFar(App& app, ecs::Entity entity, std::optional<float> zFar)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraZFar(*ew, entity, zFar))
        return false;
    if (IsActiveSceneCamera(app, entity))
        markCameraChanged(app);
    return true;
}

bool setSceneCameraAspectRatio(App& app, ecs::Entity entity, std::optional<float> aspectRatio)
{
    auto* ew = entityWorld(app);
    if (!ew || !scene::setCameraAspectRatio(*ew, entity, aspectRatio))
        return false;
    if (IsActiveSceneCamera(app, entity))
        markCameraChanged(app);
    return true;
}

void saveCurrentCamera(const App& app)
{
    assert(cameraController(app));
    cameraController(app)->saveToDefaultFile();
}

void loadCurrentCamera(App& app)
{
    assert(cameraController(app));
    cameraController(app)->loadFromDefaultFile();
}

} // namespace caustica
