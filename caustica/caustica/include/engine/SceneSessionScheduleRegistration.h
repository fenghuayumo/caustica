#pragma once

namespace caustica
{

class App;

// Explicit Update/PreRender/PostUpdate systems for SceneSession (animate, prepareRenderFrame, ECS refresh).
// Requires SceneSession registered on App via registerSceneSessionResources().
void registerSceneSessionSchedules(App& app);

} // namespace caustica
