#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <scene/View.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class App;
class PlanarView;

[[nodiscard]] uint32_t sceneCameraCount(const App& app);
[[nodiscard]] const std::vector<ecs::Entity>& sceneCameraEntities(const App& app);
[[nodiscard]] uint32_t& selectedCameraIndex(App& app);
[[nodiscard]] float cameraVerticalFOV(const App& app);
[[nodiscard]] float cameraZNear(const App& app);
[[nodiscard]] FirstPersonCamera& currentCamera(App& app);
[[nodiscard]] const FirstPersonCamera& currentCamera(const App& app);
void markCameraChanged(App& app);
[[nodiscard]] const std::shared_ptr<PlanarView>& currentView(const App& app);
[[nodiscard]] const PlanarView& view(const App& app);

[[nodiscard]] std::string currentCameraPosDirUp(const App& app);
bool setCurrentCameraPosDirUp(App& app, const std::string& val);
bool setCurrentCameraPosDirUp(
    App& app,
    const math::float3& pos,
    const math::float3& dir,
    const math::float3& up);
void setCameraVerticalFOV(App& app, float cameraFOV);
void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height);
void clearCameraIntrinsics(App& app);
void saveCurrentCamera(const App& app);
void loadCurrentCamera(App& app);

} // namespace caustica
