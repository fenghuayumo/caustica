#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>

#include "EditorLaunch.h"
#include "EditorHost.h"

#include <render/passes/debug/Korgi.h>

#include <cstring>
#include <cstdio>
#include <memory>

#ifdef _WIN32
#include <engine/SplashScreen.h>
#include <core/log.h>
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace
{

#ifdef _WIN32
LONG WINAPI CausticaUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\'))
        slash[1] = '\0';
    else
        path[0] = '\0';
    strncat_s(path, "caustica_crash.log", _TRUNCATE);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") == 0 && f)
    {
        const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        const void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        fprintf(f, "caustica crash\nExceptionCode=0x%08lX\nExceptionAddress=%p\n",
                static_cast<unsigned long>(code), addr);

        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(process, nullptr, TRUE))
        {
            CONTEXT ctx = *info->ContextRecord;
            STACKFRAME64 frame = {};
            DWORD machine = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = ctx.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            for (int i = 0; i < 64; ++i)
            {
                if (!StackWalk64(machine, process, thread, &frame, &ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (frame.AddrPC.Offset == 0)
                    break;

                char nameBuf[sizeof(SYMBOL_INFO) + 256] = {};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(nameBuf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisp = 0;

                if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym))
                {
                    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line))
                        fprintf(f, "#%02d %s +0x%llx  %s:%lu\n", i, sym->Name,
                                static_cast<unsigned long long>(disp), line.FileName, line.LineNumber);
                    else
                        fprintf(f, "#%02d %s +0x%llx\n", i, sym->Name,
                                static_cast<unsigned long long>(disp));
                }
                else
                {
                    fprintf(f, "#%02d 0x%llx\n", i, static_cast<unsigned long long>(frame.AddrPC.Offset));
                }
            }
            SymCleanup(process);
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

bool WantsHeadlessStartup(int argc, const char* const* argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--noWindow") == 0 || std::strcmp(argv[i], "--nonInteractive") == 0)
            return true;
    }
    return false;
}

#ifdef _WIN32
SplashScreen* g_activeSplash = nullptr;

void StopSplashBeforeGpuInit()
{
    if (g_activeSplash)
    {
        g_activeSplash->stop();
        g_activeSplash = nullptr;
    }
}
#endif

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int argc, char** argv)
#endif
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(CausticaUnhandledExceptionFilter);

    // Allocate a console for engine logs. Release starts hidden; Debug starts visible.
    // F1 toggles visibility. Headless/--noWindow forces visible in EditorStartup.
#if defined(_DEBUG)
    caustica::initNativeConsole(/*visibleByDefault=*/true);
#else
    caustica::initNativeConsole(/*visibleByDefault=*/false);
#endif

    if (WantsHeadlessStartup(__argc, (const char**)__argv))
        caustica::setNativeConsoleVisible(true);

    SplashScreen splashScreen;
    if (!WantsHeadlessStartup(__argc, (const char**)__argv))
    {
        splashScreen.start(L"loading_splash.png");
        g_activeSplash = &splashScreen;
    }
#endif

    caustica::initializeAppPlatform();

    caustica::editor::EditorHost host;

#ifdef _WIN32
    auto engine = caustica::editor::createEditorEngine(
        host, __argc, (const char**)__argv, StopSplashBeforeGpuInit);
#else
    auto engine = caustica::editor::createEditorEngine(
        host, argc, const_cast<const char* const*>(argv));
#endif

    if (!engine)
    {
        caustica::shutdownAppPlatform();
        korgi::shutdown();
#ifdef _WIN32
        caustica::shutdownNativeConsole();
#endif
        return 1;
    }

    const int exitCode = caustica::runEngineApp(std::move(engine));

    korgi::shutdown();
#ifdef _WIN32
    caustica::shutdownNativeConsole();
#endif
    return exitCode;
}
