#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace caustica { class App; }

class PythonScripting
{
public:
    explicit PythonScripting(caustica::App& app);
    ~PythonScripting();

    PythonScripting(const PythonScripting&)            = delete;
    PythonScripting& operator=(const PythonScripting&) = delete;

    bool Initialize();

    [[nodiscard]] bool IsInitialized() const { return m_initialized; }

    void QueueScriptFile(const std::filesystem::path& scriptPath);
    void QueueScriptString(std::string code, std::string label = "<inline>");

    void ProcessPendingScripts();

    std::string ConsumeOutputLog();

    bool ExecuteImmediate(const std::string& code, const std::string& label = "<inline>");

private:
    struct PendingScript
    {
        bool        isFile = false;
        std::string body;
        std::string label;
    };

    bool RunPendingLocked(const PendingScript& script);

    caustica::App& m_app;
    bool           m_initialized = false;

    std::mutex                 m_mutex;
    std::vector<PendingScript> m_queue;
    std::string                m_outputLog;
};
