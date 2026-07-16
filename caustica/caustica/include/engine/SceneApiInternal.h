#pragma once

#include <string>

namespace caustica
{

class App;
class CameraController;

namespace detail
{

[[nodiscard]] CameraController* sessionCamera(App& app);
void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload);

} // namespace detail
} // namespace caustica
