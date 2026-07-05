#pragma once

namespace caustica
{

class App;

// Register each Engine subsystem's frame callbacks as named AppSchedule systems.
void registerSubsystemSchedules(App& app);

} // namespace caustica
