#pragma once

// Engine-internal schedule wiring for GPU render shutdown / bind.
// Hosts must not include this.

namespace caustica
{

class App;

void registerGpuRenderSchedules(App& app);

} // namespace caustica
