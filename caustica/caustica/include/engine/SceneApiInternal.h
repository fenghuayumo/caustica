#pragma once

#include <string>

class SceneManager;

namespace caustica
{

class App;
class CameraController;

namespace detail
{

[[nodiscard]] CameraController* sessionCamera(App& app);
[[nodiscard]] const CameraController* sessionCamera(const App& app);

// Load / switch / structure-edit only. Prefer SceneQuery / activeScene for reads.
[[nodiscard]] ::SceneManager* sessionManager(App& app);
[[nodiscard]] ::SceneManager* sessionManager(const App& app);

void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload);

} // namespace detail
} // namespace caustica
