#pragma once

namespace caustica
{

class App;
struct SceneSessionConfig;

// Register scene/path-tracer session state on App (non-owning refs from host).
void registerSceneSessionResources(App& app, const SceneSessionConfig& config);

} // namespace caustica
