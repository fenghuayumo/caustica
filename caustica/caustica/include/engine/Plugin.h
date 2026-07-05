#pragma once

namespace caustica
{

class App;

// Bevy-style plugin: register subsystems and resources during App startup.
struct Plugin
{
    virtual ~Plugin() = default;
    virtual void build(App& app) = 0;
};

} // namespace caustica
