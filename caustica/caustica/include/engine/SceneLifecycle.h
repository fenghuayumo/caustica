#pragma once

#include <string>

namespace caustica
{

class App;

// Bind session-camera side effects (accumulation reset / splat temporal reset) to WorldRenderer.
void bindSessionCameraSideEffects(App& app);
void initStreamlineAndWindow(App& app);
void initializeScene(App& app, const std::string& preferredScene);
void setCurrentScene(App& app, const std::string& sceneName, bool forceReload = false);

void onSceneLoaded(App& app);
void onSceneUnloading(App& app);

void collectUncompressedTextures(App& app);
[[nodiscard]] bool hasAsyncLoadingInProgress(const App& app);

} // namespace caustica
