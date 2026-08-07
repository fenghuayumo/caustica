#pragma once

#include <string>

namespace caustica
{

class App;

// Bind CameraController side effects (accumulation reset / splat temporal reset) to WorldRenderer.
void bindCameraControllerSideEffects(App& app);
void initStreamlineAndWindow(App& app);
void initializeScene(App& app, const std::string& preferredScene);
void setCurrentScene(App& app, const std::string& sceneName, bool forceReload = false);

void onSceneLoaded(App& app);
void onSceneUnloading(App& app);

// Advance LoadSession (budgeted GPU bind). Safe to call every frame; never blocks Logic on RT.
void tickLoadSession(App& app);

// Deprecated alias — prefer tickLoadSession (ADR 0001 P3).
inline void tickSceneGpuBind(App& app) { tickLoadSession(app); }

void collectUncompressedTextures(App& app);
[[nodiscard]] bool hasAsyncLoadingInProgress(const App& app);

void sceneSwitchTrace(const char* fmt, ...);

} // namespace caustica
