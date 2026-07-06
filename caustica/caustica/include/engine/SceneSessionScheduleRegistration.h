#pragma once

namespace caustica
{

class App;

// Registers the built-in SceneSession plugin systems.
// Requires SceneSession registered on App via registerSceneSessionResources().
void registerSceneSessionSchedules(App& app);

} // namespace caustica
