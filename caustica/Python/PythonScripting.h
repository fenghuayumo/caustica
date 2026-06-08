/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// PythonScripting.h
// -----------------
// Embeds the CPython interpreter into the caustica process and exposes
// a dedicated `caustica` module (via nanobind) that allows Python scripts
// to read and tweak scene related state at runtime: materials, lights,
// the environment map, the path-tracer settings, the camera, etc.
//
// The interpreter lives on the same thread as the renderer and runs
// scripts queued up via QueueScriptFile/QueueScriptString. The Sample
// drains the queue once per frame, after the scene is fully loaded, so
// that the Python code observes a consistent view of the scene graph.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Sample;

class PythonScripting
{
public:
    explicit PythonScripting(Sample& app);
    ~PythonScripting();

    // Non-copyable / non-movable: owns the embedded CPython runtime.
    PythonScripting(const PythonScripting&)            = delete;
    PythonScripting& operator=(const PythonScripting&) = delete;

    // Initialize the interpreter the first time it is called. Safe to call
    // multiple times - subsequent calls are no-ops. Must be called from the
    // main (renderer) thread.
    bool Initialize();

    // Returns true if the embedded interpreter is up and running.
    bool IsInitialized() const { return m_initialized; }

    // Queue a Python source file or expression for execution on the next
    // ProcessPendingScripts() call. Both paths are exception-safe and never
    // throw; failures are logged via donut::log.
    void QueueScriptFile(const std::filesystem::path& scriptPath);
    void QueueScriptString(std::string code, std::string label = "<inline>");

    // Run any queued scripts on the calling thread. Must be called from the
    // main thread (between renders), after Sample::SceneLoaded() has run.
    void ProcessPendingScripts();

    // Returns the most recent stdout/stderr output captured from Python.
    // Cleared each time the UI consumes it, see SampleUI for usage.
    std::string ConsumeOutputLog();

    // Test helper - synchronously execute a string and return success.
    bool ExecuteImmediate(const std::string& code, const std::string& label = "<inline>");

private:
    struct PendingScript
    {
        bool        isFile = false;
        std::string body;
        std::string label;
    };

    bool RunPendingLocked(const PendingScript& script);

    Sample&                         m_app;
    bool                            m_initialized = false;

    std::mutex                      m_mutex;
    std::vector<PendingScript>      m_queue;
    std::string                     m_outputLog;
};
