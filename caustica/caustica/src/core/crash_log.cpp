#include <core/crash_log.h>

#ifdef _WIN32

#include <cstdio>
#include <cstring>
#include <mutex>

#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    std::once_flag g_installOnce;

    void CurrentModuleDirectory(char (&path)[MAX_PATH])
    {
        // Resolve from an address inside this translation unit so the log
        // lands next to the binary that contains this code (exe or pyd).
        HMODULE module = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&CurrentModuleDirectory),
                &module))
        {
            const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                if (char* slash = strrchr(path, '\\'))
                {
                    slash[1] = '\0';
                    return;
                }
            }
        }
        path[0] = '\0';
    }

    LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* info)
    {
        char path[MAX_PATH] = {};
        CurrentModuleDirectory(path);
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
                frame.AddrPC.Offset = ctx.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;

                for (int i = 0; i < 64; ++i)
                {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
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
            fprintf(stderr, "[caustica] crash log written to %s\n", path);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void caustica::installUnhandledExceptionLogger()
{
    std::call_once(g_installOnce, []() {
        SetUnhandledExceptionFilter(UnhandledExceptionLogger);
    });
}

#else // _WIN32

void caustica::installUnhandledExceptionLogger() {}

#endif // _WIN32
