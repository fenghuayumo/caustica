#pragma once

namespace caustica
{

class App;

// Plugin lifecycle:
//   build()             — register subsystems and resources
//   configureSchedules() — register AppSchedule systems (after build, before engine init)
struct Plugin
{
    virtual ~Plugin() = default;

    virtual void build(App& app) = 0;
    virtual void configureSchedules(App& app) {}
};

} // namespace caustica
