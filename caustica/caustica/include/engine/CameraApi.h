#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <scene/View.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caustica
{

class App;
class ViewInfo;

struct CameraPose
{
    math::float3 position = { 0.f, 0.f, 0.f };
    math::float3 direction = { 0.f, 0.f, -1.f };
    math::float3 up = { 0.f, 1.f, 0.f };
};

[[nodiscard]] uint32_t sceneCameraCount(const App& app);
[[nodiscard]] const std::vector<ecs::Entity>& sceneCameraEntities(const App& app);
[[nodiscard]] uint32_t selectedCameraIndex(const App& app);
// Index 0 is the free camera. Scene-camera indices follow
// sceneCameraEntities(); only perspective scene cameras are selectable by the
// active-camera API until an orthographic controller path is implemented.
bool setSelectedCameraIndex(App& app, uint32_t index);

// NullEntity when the free / controller camera (index 0) is selected.
[[nodiscard]] ecs::Entity activeSceneCameraEntity(const App& app);
[[nodiscard]] bool activeCameraIsFree(const App& app);
[[nodiscard]] std::string activeCameraPath(const App& app);
[[nodiscard]] std::string activeCameraName(const App& app);
// NullEntity selects the free camera. Returns false if `entity` is not a registered scene camera.
bool setActiveCamera(App& app, ecs::Entity entity);
bool setActiveCameraByPath(App& app, const std::string& path);

[[nodiscard]] float cameraVerticalFOV(const App& app);
[[nodiscard]] float cameraZNear(const App& app);
[[nodiscard]] FirstPersonCamera& currentCamera(App& app);
[[nodiscard]] const FirstPersonCamera& currentCamera(const App& app);
void markCameraChanged(App& app);
[[nodiscard]] const std::shared_ptr<ViewInfo>& currentView(const App& app);
[[nodiscard]] const ViewInfo& view(const App& app);

[[nodiscard]] std::string currentCameraPosDirUp(const App& app);
[[nodiscard]] CameraPose currentCameraPose(const App& app);
bool setCurrentCameraPose(App& app, const CameraPose& pose);
bool setCurrentCameraPosDirUp(App& app, const std::string& val);
bool setCurrentCameraPosDirUp(
    App& app,
    const math::float3& pos,
    const math::float3& dir,
    const math::float3& up);
bool setCameraVerticalFOV(App& app, float cameraFOV);
bool setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height);
bool clearCameraIntrinsics(App& app);

bool setSceneCameraVerticalFOV(App& app, ecs::Entity entity, float radians);
[[nodiscard]] float sceneCameraVerticalFOV(const App& app, ecs::Entity entity);
// Camera view-space look (Z-flip). Do not aim a camera with entity world TRS.
bool setSceneCameraLookTo(
    App& app,
    ecs::Entity entity,
    const math::float3& pos,
    const math::float3& dir,
    const math::float3& up);
bool setSceneCameraIntrinsics(
    App& app,
    ecs::Entity entity,
    float fx,
    float fy,
    float cx,
    float cy,
    float width,
    float height);
bool clearSceneCameraIntrinsics(App& app, ecs::Entity entity);
bool setSceneCameraZNear(App& app, ecs::Entity entity, float zNear);
bool setSceneCameraZFar(App& app, ecs::Entity entity, std::optional<float> zFar);
bool setSceneCameraAspectRatio(App& app, ecs::Entity entity, std::optional<float> aspectRatio);

void saveCurrentCamera(const App& app);
void loadCurrentCamera(App& app);

} // namespace caustica
