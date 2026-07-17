#pragma once

namespace caustica
{

class App;

// Plugin lifecycle:
//   build()                — register resources and owned services
//   configureSchedules()   — register AppSchedule systems (before default schedules)
//   configureLateSchedules() — register systems after default + scene runtime schedules
struct Plugin
{
    virtual ~Plugin() = default;

    virtual void build(App& app) {}
    virtual void configureSchedules(App& app) {}
    virtual void configureLateSchedules(App& app) {}
};

} // namespace caustica
