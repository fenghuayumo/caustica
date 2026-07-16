#pragma once

namespace caustica
{

class App;
struct SceneAppConfig;

// Register scene/path-tracer resources on App (non-owning refs from host).
void registerSceneAppResources(App& app, const SceneAppConfig& config);

} // namespace caustica
