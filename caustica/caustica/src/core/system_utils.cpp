#include <core/system_utils.h>
#include <core/file_utils.h>
#include <core/format.h>

#include <cassert>
#include <atomic>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

namespace caustica
{

// =============================================================================
// systemShell
// =============================================================================

std::tuple<int, std::string, std::string> systemShell(
    const std::string& command, bool useCmd, bool blockOnExecution)
{
    assert(blockOnExecution); // non-blocking not implemented

    int resultValue = 0;
    std::string resultString;
    std::string resultErrorString;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hReadOut = NULL, hWriteOut = NULL, hReadErr = NULL, hWriteErr = NULL;

    CreatePipe(&hReadOut, &hWriteOut, &sa, 0);
    CreatePipe(&hReadErr, &hWriteErr, &sa, 0);
    SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hReadErr, HANDLE_FLAG_INHERIT, 0);

    bool showConsole = false;

    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    if (!showConsole)
    {
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWriteOut;
        si.hStdError = hWriteErr;
        si.wShowWindow = SW_HIDE;
    }

    std::string cmd = command;
    if (useCmd)
        cmd = "cmd /C \"" + command + "\"";

    BOOL success = CreateProcessA(NULL, cmd.data(), NULL, &sa, TRUE,
        showConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi);
    CloseHandle(hWriteOut);
    CloseHandle(hWriteErr);

    if (success)
    {
        char buffer[4096];
        DWORD bytesRead;

        while (true)
        {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 50);
            DWORD available = 0;

            while (PeekNamedPipe(hReadOut, NULL, 0, NULL, &available, NULL) && available > 0)
            {
                if (ReadFile(hReadOut, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
                {
                    buffer[bytesRead] = '\0';
                    resultString += buffer;
                }
                else { assert(false); break; }
            }

            while (PeekNamedPipe(hReadErr, NULL, 0, NULL, &available, NULL) && available > 0)
            {
                if (ReadFile(hReadErr, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
                {
                    buffer[bytesRead] = '\0';
                    resultErrorString += buffer;
                }
                else { assert(false); break; }
            }

            if (waitResult == WAIT_OBJECT_0)
                break;
        }

        GetExitCodeProcess(pi.hProcess, reinterpret_cast<DWORD*>(&resultValue));
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else
    {
        resultErrorString = "CreateProcess failed.";
    }

    CloseHandle(hReadOut);
    CloseHandle(hReadErr);

#else
    static std::atomic<int> fileLogIndexStorage(0);
    int uniqueIndex = fileLogIndexStorage.fetch_add(1);

    auto tempLogFile = std::filesystem::temp_directory_path() /
        stringFormat("CAUSTICA_out_%d_%d.txt", getpid(), uniqueIndex);
    auto tempErrLogFile = std::filesystem::temp_directory_path() /
        stringFormat("CAUSTICA_err_%d_%d.txt", getpid(), uniqueIndex);

    std::string startCmd = command + " > \"" + tempLogFile.string() + "\"" +
        " 2> \"" + tempErrLogFile.string() + "\"";

    resultValue = std::system(startCmd.c_str());

    if (std::filesystem::exists(tempLogFile))
    {
        resultString = stringLoadFromFile(tempLogFile);
        std::filesystem::remove(tempLogFile);
    }
    if (std::filesystem::exists(tempErrLogFile))
    {
        resultErrorString = stringLoadFromFile(tempErrLogFile);
        std::filesystem::remove(tempErrLogFile);
    }
#endif

    return std::make_tuple(resultValue, resultString, resultErrorString);
}

} // namespace caustica
