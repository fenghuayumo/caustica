#pragma once

#include <scene/camera/Camera.h>
#include <scene/View.h>

#include <memory>
#include <string>

namespace caustica
{

class App;
class PlanarView;

[[nodiscard]] uint sceneCameraCount(const App& app);
[[nodiscard]] uint& selectedCameraIndex(App& app);
[[nodiscard]] float cameraVerticalFOV(const App& app);
[[nodiscard]] const FirstPersonCamera& currentCamera(const App& app);
[[nodiscard]] const std::shared_ptr<PlanarView>& currentView(const App& app);
[[nodiscard]] const PlanarView& view(const App& app);

[[nodiscard]] std::string currentCameraPosDirUp(const App& app);
bool setCurrentCameraPosDirUp(App& app, const std::string& val);
void setCameraVerticalFOV(App& app, float cameraFOV);
void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height);
void clearCameraIntrinsics(App& app);
void saveCurrentCamera(const App& app);
void loadCurrentCamera(App& app);

} // namespace caustica
