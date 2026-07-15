#pragma once

#include <functional>

namespace caustica
{
    enum class Severity
    {
        None = 0,
        Debug,
        Info,
        Warning,
        Error,
        Fatal
    };

	typedef std::function<void(Severity, char const*)> Callback;

    void setMinSeverity(Severity severity);
    void setCallback(Callback func);
	Callback getCallback();
    void resetCallback();

    // Windows: enables or disables future log messages to be shown as MessageBox'es.
    // This is the default mode.
    // Linux: no effect, log messages are always printed to the console.
    void enableOutputToMessageBox(bool enable);

    // Windows: enables or disables future log messages to be printed to stdout or stderr, depending on severity.
    // Linux: no effect, log messages are always printed to the console.
    void enableOutputToConsole(bool enable);

    // Windows: enables or disables future log messages to be printed using OutputDebugString.
    // Linux: no effect, log messages are always printed to the console.
    void enableOutputToDebug(bool enable);

    // Windows: sets the caption to be used by the error message boxes.
    // Linux: no effect.
    void setErrorMessageCaption(const char* caption);

    // Equivalent to the following sequence of calls:
    // - enableOutputToConsole(true);
    // - enableOutputToDebug(true);
    // - enableOutputToMessageBox(false);
    void consoleApplicationMode();

    // Windows WIN32 subsystem: allocate a console once, redirect stdio, and enable
    // log output to it. visibleByDefault=false hides the window (Release default).
    // No-op on non-Windows. Safe to call more than once.
    void initNativeConsole(bool visibleByDefault);
    void shutdownNativeConsole();
    void setNativeConsoleVisible(bool visible);
    [[nodiscard]] bool isNativeConsoleVisible();
    bool toggleNativeConsoleVisible();

    void message(Severity severity, const char* fmt...);
    void debug(const char* fmt...);
    void info(const char* fmt...);
    void warning(const char* fmt...);
    void error(const char* fmt...);
    void fatal(const char* fmt...);
}
