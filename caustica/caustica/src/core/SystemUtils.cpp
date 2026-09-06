#include <core/SystemUtils.h>
#include <core/FileUtils.h>
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
    SECURITY_ATTRIBUTES securityAttributes = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE stdoutReadHandle = NULL, stdoutWriteHandle = NULL, stderrReadHandle = NULL, stderrWriteHandle = NULL;

    CreatePipe(&stdoutReadHandle, &stdoutWriteHandle, &securityAttributes, 0);
    CreatePipe(&stderrReadHandle, &stderrWriteHandle, &securityAttributes, 0);
    SetHandleInformation(stdoutReadHandle, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrReadHandle, HANDLE_FLAG_INHERIT, 0);

    bool showConsole = false;

    STARTUPINFOA startupInfo = { 0 };
    PROCESS_INFORMATION processInformation = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    if (!showConsole)
    {
        startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startupInfo.hStdOutput = stdoutWriteHandle;
        startupInfo.hStdError = stderrWriteHandle;
        startupInfo.wShowWindow = SW_HIDE;
    }

    std::string commandLine = command;
    if (useCmd)
        commandLine = "cmd /C \"" + command + "\"";

    BOOL processCreated = CreateProcessA(NULL, commandLine.data(), NULL, &securityAttributes, TRUE,
        showConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
        NULL, NULL, &startupInfo, &processInformation);
    CloseHandle(stdoutWriteHandle);
    CloseHandle(stderrWriteHandle);

    if (processCreated)
    {
        char buffer[4096];
        DWORD bytesRead;

        while (true)
        {
            DWORD waitResult = WaitForSingleObject(processInformation.hProcess, 50);
            DWORD available = 0;

            while (PeekNamedPipe(stdoutReadHandle, NULL, 0, NULL, &available, NULL) && available > 0)
            {
                if (ReadFile(stdoutReadHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
                {
                    buffer[bytesRead] = '\0';
                    resultString += buffer;
                }
                else { assert(false); break; }
            }

            while (PeekNamedPipe(stderrReadHandle, NULL, 0, NULL, &available, NULL) && available > 0)
            {
                if (ReadFile(stderrReadHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
                {
                    buffer[bytesRead] = '\0';
                    resultErrorString += buffer;
                }
                else { assert(false); break; }
            }

            if (waitResult == WAIT_OBJECT_0)
                break;
        }

        GetExitCodeProcess(processInformation.hProcess, reinterpret_cast<DWORD*>(&resultValue));
        CloseHandle(processInformation.hProcess);
        CloseHandle(processInformation.hThread);
    }
    else
    {
        resultErrorString = "CreateProcess failed.";
    }

    CloseHandle(stdoutReadHandle);
    CloseHandle(stderrReadHandle);

#else
    static std::atomic<int> fileLogIndexStorage(0);
    int uniqueIndex = fileLogIndexStorage.fetch_add(1);

    auto tempLogFile = std::filesystem::temp_directory_path() /
        stringFormat("CAUSTICA_out_%d_%d.txt", getpid(), uniqueIndex);
    auto tempErrLogFile = std::filesystem::temp_directory_path() /
        stringFormat("CAUSTICA_err_%d_%d.txt", getpid(), uniqueIndex);

    std::string startCommand = command + " > \"" + tempLogFile.string() + "\"" +
        " 2> \"" + tempErrLogFile.string() + "\"";

    resultValue = std::system(startCommand.c_str());

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
