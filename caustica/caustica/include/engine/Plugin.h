#pragma once

namespace caustica
{

class App;

// Plugin: register subsystems via build(), schedule systems via addSystem().
struct Plugin
{
    virtual ~Plugin() = default;
    virtual void build(App& app) = 0;
};

} // namespace caustica
