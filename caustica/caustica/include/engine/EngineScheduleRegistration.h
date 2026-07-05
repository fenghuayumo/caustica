#pragma once

namespace caustica
{

class App;

// Idempotent app infrastructure schedules (events, DPI, GPU dispatch).
// Subsystem frame callbacks are registered separately via registerSubsystemSchedules().
void registerEngineScheduleBridge(App& app);

} // namespace caustica
