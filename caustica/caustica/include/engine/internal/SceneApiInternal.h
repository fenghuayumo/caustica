#pragma once

#include <string>

class SceneManager;

namespace caustica
{

class App;

namespace detail
{

// Load / switch / structure-edit only. Prefer SceneQuery / activeScene for reads.
[[nodiscard]] ::SceneManager* sessionManager(App& app);
[[nodiscard]] ::SceneManager* sessionManager(const App& app);

void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload);

// Survives GetOpenFileName breaking stdout — also appends bin/scene_switch.log.
void sceneSwitchTrace(const char* fmt, ...);

} // namespace detail
} // namespace caustica
