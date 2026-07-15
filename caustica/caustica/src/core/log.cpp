#include <core/log.h>
#include <cstdio>
#include <cstdarg>
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
        snprintf(buf, std::size(buf), "%s: %s", severityText, message);

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
                if (severity == Severity::Error || severity == Severity::Fatal)
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
        g_Callback = func ? std::move(func) : Callback(&DefaultCallback);
    }

	Callback getCallback()
	{
		return g_Callback;
	}

    void resetCallback()
    {
        g_Callback = &DefaultCallback;
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
        setNativeConsoleVisible(visibleByDefault);

        info("Native console ready (F1 toggles visibility)");
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

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(severity, buffer);

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

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(Severity::Debug, buffer);

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

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(Severity::Info, buffer);

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

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(Severity::Warning, buffer);

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

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(Severity::Error, buffer);

        va_end(args);
    }

    void fatal(const char* fmt...)
    {
        char buffer[g_MessageBufferSize];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, std::size(buffer), fmt, args);

        (g_Callback ? g_Callback : Callback(&DefaultCallback))(Severity::Fatal, buffer);

        va_end(args);
    }
}
