#pragma once

namespace caustica
{

class App;
struct SceneRuntimeSubsystemConfig;

// Register scene/path-tracer session state on App (non-owning refs from host).
void registerSceneSessionResources(App& app, const SceneRuntimeSubsystemConfig& config);

} // namespace caustica
