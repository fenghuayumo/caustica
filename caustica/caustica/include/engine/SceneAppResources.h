#pragma once

namespace caustica
{

class App;

// Insert scene/path-tracer resources that EngineApp does not already own.
void registerSceneAppResources(App& app);

} // namespace caustica
