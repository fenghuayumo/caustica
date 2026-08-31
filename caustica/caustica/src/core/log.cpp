#include <core/log.h>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#if _WIN32
#include <share.h>
#endif
#include <iterator>
#include <mutex>
#include <utility>
#include <string>
#if _WIN32
#include <Windows.h>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#endif

namespace caustica
{
    static constexpr size_t g_MessageBufferSize = 4096;

    static std::string g_ErrorMessageCaption = "Error";

#if _WIN32
    static bool g_OutputToMessageBox = true;
    static bool g_OutputToDebug = true;
    static bool g_OutputToConsole = false;
#else
    static bool g_OutputToMessageBox = false;
    static bool g_OutputToDebug = false;
    static bool g_OutputToConsole = true;
#endif

    static std::mutex g_LogMutex;
    static std::mutex g_CallbackMutex;
    
    // Seconds since the first log line. Opt-in via CAUSTICA_LOG_TIMESTAMPS=1 so the
    // default output format stays byte-identical for anything parsing it. Startup
    // cost is otherwise invisible: most startup work logs nothing, and the one
    // existing timestamped trace (sceneSwitchTrace) only starts at the first scene
    // switch, well after device and pipeline bring-up.
    bool timestampsEnabled()
    {
        static const bool enabled = []() {
            const char* value = std::getenv("CAUSTICA_LOG_TIMESTAMPS");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        }();
        return enabled;
    }

    double secondsSinceFirstLog()
    {
        static const auto origin = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
    }

    // Optional file sink, enabled with CAUSTICA_LOG_FILE=<path>. Needed because
    // initNativeConsole() freopen()s stdout onto CONOUT$, which silently detaches any
    // redirection the launcher set up —so piping the process to a file captures only
    // the lines emitted before the console is created, which is a small and
    // misleading prefix of startup.
    FILE* logFileSink()
    {
        static FILE* sink = []() -> FILE* {
            const char* path = std::getenv("CAUSTICA_LOG_FILE");
            if (path == nullptr || path[0] == '\0')
                return nullptr;
#if _WIN32
            // Share for reading: fopen() denies all other access on Windows, which makes
            // the log impossible to tail while the app is running — and startup traces
            // are most useful exactly while you are waiting on startup.
            return _fsopen(path, "w", _SH_DENYWR);
#else
            return fopen(path, "w");
#endif
        }();
        return sink;
    }

    void DefaultCallback(Severity severity, const char* message)
    {
        const char* severityText = "";
        switch (severity)
        {
        case Severity::Debug: severityText = "DEBUG";  break;
        case Severity::Info: severityText = "INFO";  break;
        case Severity::Warning: severityText = "WARNING"; break;
        case Severity::Error: severityText = "ERROR"; break;
        case Severity::Fatal: severityText = "FATAL ERROR"; break;
		default:
			break;
        }

        char buf[g_MessageBufferSize];
        if (timestampsEnabled())
        {
            snprintf(buf, std::size(buf), "[%9.3fs] %s: %s",
                secondsSinceFirstLog(), severityText, message);
        }
        else
        {
            snprintf(buf, std::size(buf), "%s: %s", severityText, message);
        }

        {
            std::lock_guard<std::mutex> lockGuard(g_LogMutex);

#if _WIN32
            if (g_OutputToDebug)
            {
                OutputDebugStringA(buf);
                OutputDebugStringA("\n");
            }

            if (g_OutputToMessageBox)
            {
                // Error callbacks may originate on the render thread or while
                // the main window is being destroyed. A modal Win32 dialog
                // pumps window messages and can re-enter editor input against
                // partially torn-down state; DRED also emits many error lines.
                // Keep fatal startup failures visible, but ordinary errors are
                // diagnostic output and must never become a modal GPU stall.
                if (severity == Severity::Fatal)
                {
                    MessageBoxA(0, buf, g_ErrorMessageCaption.c_str(), MB_ICONERROR);
                }
            }

#endif
            if (g_OutputToConsole)
            {
                if (severity == Severity::Error || severity == Severity::Fatal)
                {
                    fprintf(stderr, "%s\n", buf);
                    fflush(stderr);
                }
                else
                {
                    fprintf(stdout, "%s\n", buf);
                    fflush(stdout);
                }
            }
        }

        if (severity == Severity::Fatal)
            abort();
    }

    void setErrorMessageCaption(const char* caption)
    {
        g_ErrorMessageCaption = (caption) ? caption : "";
    }

    static Callback g_Callback = &DefaultCallback;
    static Severity g_MinSeverity = Severity::Info;

    void setMinSeverity(Severity severity)
    {
        g_MinSeverity = severity;
    }

    void setCallback(Callback func)
    {
		std::lock_guard<std::mutex> lockGuard(g_CallbackMutex);
        g_Callback = func ? std::move(func) : Callback(&DefaultCallback);
    }

	Callback getCallback()
	{
		std::lock_guard<std::mutex> lockGuard(g_CallbackMutex);
		return g_Callback;
	}

    void resetCallback()
    {
		std::lock_guard<std::mutex> lockGuard(g_CallbackMutex);
        g_Callback = &DefaultCallback;
    }

    static Callback snapshotCallback()
    {
		std::lock_guard<std::mutex> lockGuard(g_CallbackMutex);
		return g_Callback ? g_Callback : Callback(&DefaultCallback);
	}

    // Deliberately outside the callback chain. ImGui_Console installs a callback that
    // does not chain to the previous one, so from the moment the in-game console is
    // created every message goes only into its ring buffer —stdout, OutputDebugString
    // and DebugView all go silent. That makes startup and shutdown impossible to
    // diagnose from a terminal, so the file sink is fed here instead.
    static void writeFileSink(Severity severity, const char* message)
    {
        FILE* sink = logFileSink();
        if (sink == nullptr)
            return;

        const char* severityText = "";
        switch (severity)
        {
        case Severity::Debug: severityText = "DEBUG"; break;
        case Severity::Info: severityText = "INFO"; break;
        case Severity::Warning: severityText = "WARNING"; break;
        case Severity::Error: severityText = "ERROR"; break;
        case Severity::Fatal: severityText = "FATAL ERROR"; break;
        default: break;
        }

        std::lock_guard<std::mutex> lockGuard(g_LogMutex);
        if (timestampsEnabled())
            fprintf(sink, "[%9.3fs] %s: %s\n", secondsSinceFirstLog(), severityText, message);
        else
            fprintf(sink, "%s: %s\n", severityText, message);
        fflush(sink);
    }
    
    void enableOutputToMessageBox(bool enable)
    {
        g_OutputToMessageBox = enable;
    }
    
    void enableOutputToConsole(bool enable)
    {
        g_OutputToConsole = enable;
    }
    
    void enableOutputToDebug(bool enable)
    {
        g_OutputToDebug = enable;
    }

    void consoleApplicationMode()
    {
        g_OutputToConsole = true;
        g_OutputToDebug = true;
        g_OutputToMessageBox = false;
    }

#if _WIN32
    namespace
    {
        HWND g_NativeConsoleHwnd = nullptr;
        bool g_NativeConsoleVisible = false;
        bool g_NativeConsoleInitialized = false;
        bool g_NativeConsoleOwned = false; // true when we AllocConsole()'d (not AttachConsole)

        void RedirectStdioToConsole()
        {
            FILE* unused = nullptr;
            freopen_s(&unused, "CONOUT$", "w", stdout);
            freopen_s(&unused, "CONOUT$", "w", stderr);
            freopen_s(&unused, "CONIN$", "r", stdin);
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
            std::ios::sync_with_stdio(true);
        }
    }

    void initNativeConsole(bool visibleByDefault)
    {
        if (g_NativeConsoleInitialized)
        {
            setNativeConsoleVisible(visibleByDefault);
            return;
        }

        // Prefer attaching to a parent console (launched from cmd); else allocate one.
        const bool attachedToParent = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        if (!attachedToParent && !AllocConsole())
            return;

        g_NativeConsoleHwnd = GetConsoleWindow();
        if (!g_NativeConsoleHwnd)
            return;

        SetConsoleTitleW(L"caustica console");
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        RedirectStdioToConsole();

        g_NativeConsoleInitialized = true;
        g_NativeConsoleOwned = !attachedToParent;
        g_OutputToConsole = true;
        // Keep OutputDebugString on so scene-switch logs remain visible in DebugView
        // even if a common file dialog breaks CRT→console redirection.
        g_OutputToDebug = true;
        setNativeConsoleVisible(visibleByDefault);

        info("Native console ready (F1 toggles visibility)");
    }

    void refreshNativeConsoleStdio()
    {
        if (!g_NativeConsoleInitialized)
            return;
        RedirectStdioToConsole();
        g_OutputToConsole = true;
        g_OutputToDebug = true;
    }

    void shutdownNativeConsole()
    {
        if (!g_NativeConsoleInitialized)
            return;

        g_OutputToConsole = false;

        // Detach CRT from the console before FreeConsole so exit-time stream
        // flush/close cannot heap-corrupt against a torn-down console.
        FILE* unused = nullptr;
        freopen_s(&unused, "NUL", "w", stdout);
        freopen_s(&unused, "NUL", "w", stderr);
        freopen_s(&unused, "NUL", "r", stdin);

        if (g_NativeConsoleOwned)
            FreeConsole();

        g_NativeConsoleHwnd = nullptr;
        g_NativeConsoleVisible = false;
        g_NativeConsoleOwned = false;
        g_NativeConsoleInitialized = false;
    }

    void setNativeConsoleVisible(bool visible)
    {
        if (!g_NativeConsoleHwnd)
            g_NativeConsoleHwnd = GetConsoleWindow();
        if (!g_NativeConsoleHwnd)
            return;

        ShowWindow(g_NativeConsoleHwnd, visible ? SW_SHOW : SW_HIDE);
        g_NativeConsoleVisible = visible;
    }

    bool isNativeConsoleVisible()
    {
        return g_NativeConsoleVisible;
    }

    bool toggleNativeConsoleVisible()
    {
        const bool next = !g_NativeConsoleVisible;
        setNativeConsoleVisible(next);
        return next;
    }
#else
    void initNativeConsole(bool) {}
    void shutdownNativeConsole() {}
    void refreshNativeConsoleStdio() {}
    void setNativeConsoleVisible(bool) {}
    bool isNativeConsoleVisible() { return true; }
    bool toggleNativeConsoleVisible() { return true; }
#endif

    void message(Severity severity, const char* fmt...)
    {
        if (static_cast<int>(g_MinSeverity) > static_cast<int>(severity))
            return;

        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(severity, buffer);

        snapshotCallback()(severity, buffer);

        va_end(args);
    }

    void debug(const char* fmt...)
    {
        if (static_cast<int>(g_MinSeverity) > static_cast<int>(Severity::Debug))
            return;

        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(Severity::Debug, buffer);

        snapshotCallback()(Severity::Debug, buffer);

        va_end(args);
    }

    void info(const char* fmt...)
    {
        if (static_cast<int>(g_MinSeverity) > static_cast<int>(Severity::Info))
            return;

        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(Severity::Info, buffer);

        snapshotCallback()(Severity::Info, buffer);

        va_end(args);
    }

    void warning(const char* fmt...)
    {
        if (static_cast<int>(g_MinSeverity) > static_cast<int>(Severity::Warning))
            return;

        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(Severity::Warning, buffer);

        snapshotCallback()(Severity::Warning, buffer);

        va_end(args);
    }

    void error(const char* fmt...)
    {
        if (static_cast<int>(g_MinSeverity) > static_cast<int>(Severity::Error))
            return;

        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(Severity::Error, buffer);

        snapshotCallback()(Severity::Error, buffer);

        va_end(args);
    }

    void fatal(const char* fmt...)
    {
        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        writeFileSink(Severity::Fatal, buffer);

        snapshotCallback()(Severity::Fatal, buffer);

        va_end(args);
    }
}
