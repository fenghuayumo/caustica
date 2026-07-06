#pragma once

namespace caustica
{

class App;

namespace sceneSession
{

void registerSceneLoadingPlugin(App& app);
void registerSceneAnimationPlugin(App& app);
void registerCameraPlugin(App& app);
void registerPathTracingPlugin(App& app);
void registerRenderExtractPlugin(App& app);
void registerWindowTitlePlugin(App& app);

} // namespace sceneSession

} // namespace caustica
