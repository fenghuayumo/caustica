#pragma once

// ENGINE-INTERNAL — not part of the public API (see docs/public-api.md).

namespace caustica
{

class App;

// Registers built-in Scene.* plugin systems (loading, animation, camera, extract, path trace).
// Requires scene resources via registerSceneAppResources().
void registerSceneSchedules(App& app);

} // namespace caustica
