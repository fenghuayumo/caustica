#pragma once

namespace caustica
{

class App;

// Register default AppSchedule bridge systems (events, render dispatch, etc.).
void registerEngineScheduleBridge(App& app);

} // namespace caustica
