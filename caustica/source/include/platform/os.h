#pragma once

#include <string>
#include <filesystem>
#include <array>

namespace caustica
{

enum class PowerState
{
    POWERSTATE_UNKNOWN,
    POWERSTATE_ON_BATTERY,
    POWERSTATE_NO_BATTERY,
    POWERSTATE_CHARGING,
    POWERSTATE_CHARGED
};

// Platform abstraction: operating system services.
// One instance per application lifetime, set via setInstance() before use.
class OS
{
public:
    OS()          = default;
    virtual ~OS() = default;

    // Enter the platform message loop. Calls app.init() then app.run() then app.release().
    virtual void run() = 0;

    // One-time platform initialization (register window classes, etc.)
    virtual void init() {}

    static void setInstance(OS* instance) { s_Instance = instance; }
    static OS* instance()                { return s_Instance; }

    static std::string powerStateToString(PowerState state);

    // Path to the running executable
    virtual std::string getExecutablePath() = 0;

    // Current working directory
    virtual std::string getCurrentWorkingDirectory() { return std::filesystem::current_path().string(); }

    // Path where compiled shaders are located
    virtual std::string getShaderAssetPath() { return ""; }

    // Platform UI operations
    virtual void openFileLocation(const std::filesystem::path& path) {}
    virtual void openFileExternal(const std::filesystem::path& path) {}
    virtual void openURL(const std::string& url) {}

    // Window controls
    virtual void setTitleBarColour(const std::array<float,4>& colour, bool dark = true) {}
    virtual void maximiseWindow() {}

    // Mobile only
    virtual void vibrate() const {}
    virtual void showKeyboard() {}
    virtual void hideKeyboard() {}
    virtual void delay(uint32_t usec) {}

protected:
    static OS* s_Instance;
};

} // namespace caustica
