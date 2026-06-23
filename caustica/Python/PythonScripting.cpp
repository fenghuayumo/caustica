/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "PythonScripting.h"

#if RTXPT_WITH_PYTHON

#include <core/log.h>

// nanobind & CPython
#include <nanobind/nanobind.h>
#include <Python.h>

#include "caustica.h"

namespace nb = nanobind;

// Forward declared in PythonBindings.cpp - registers the caustica module.
extern Sample* g_pythonSampleSingleton;

// Symbol defined by NB_MODULE(caustica, m) inside PythonBindings.cpp - we declare
// it manually here so we can hand it to PyImport_AppendInittab without pulling
// nanobind into the call site.
extern "C" PyObject* PyInit_caustica(void);

namespace
{
    // Captures stdout / stderr coming out of the Python interpreter so the
    // host application can show them in the on-screen log even when launched
    // without an attached console (the typical case for windowed builds).
    constexpr const char* kStdoutCaptureScript = R"PY(
import sys

class _CausticaStdoutCapture:
    def __init__(self, channel):
        self._channel = channel
        self._buffer = []

    def write(self, text):
        if text:
            self._buffer.append(text)

    def flush(self):
        pass

    def consume(self):
        out = "".join(self._buffer)
        self._buffer.clear()
        return out

if not hasattr(sys, "_caustica_capture_stdout"):
    sys._caustica_capture_stdout = _CausticaStdoutCapture("stdout")
    sys._caustica_capture_stderr = _CausticaStdoutCapture("stderr")
    sys.stdout = sys._caustica_capture_stdout
    sys.stderr = sys._caustica_capture_stderr
)PY";
}

PythonScripting::PythonScripting(Sample& app)
    : m_app(app)
{
}

PythonScripting::~PythonScripting()
{
    if (m_initialized)
    {
        // Releasing the interpreter ensures all nb::class_ owned objects are
        // collected before the surrounding C++ machinery is torn down.
        try
        {
            nb::gil_scoped_acquire gil;
            // Force a final garbage collection pass so __del__ side effects
            // run while the bindings target classes are still valid.
            PyGC_Collect();
        }
        catch (...) {}
        Py_FinalizeEx();
        m_initialized = false;
    }
    g_pythonSampleSingleton = nullptr;
}

bool PythonScripting::Initialize()
{
    if (m_initialized)
        return true;

    // Make the Sample pointer visible to nanobind bindings before init.
    g_pythonSampleSingleton = &m_app;

    // Register our embedded module before Py_Initialize so that scripts can
    // immediately `import caustica` without touching sys.path.
    if (PyImport_AppendInittab("caustica", PyInit_caustica) != 0)
    {
        donut::log::error("PythonScripting: PyImport_AppendInittab failed");
        return false;
    }

    Py_InitializeEx(0);
    if (!Py_IsInitialized())
    {
        donut::log::error("PythonScripting: Py_InitializeEx failed");
        return false;
    }

    // Hook up stdout/stderr redirection so script output ends up in our log.
    {
        nb::gil_scoped_acquire gil;
        try
        {
            PyRun_SimpleString(kStdoutCaptureScript);
            // Pre-import caustica so the user does not need to do it explicitly
            // for inline expressions (interactive console, etc.).
            PyRun_SimpleString("import caustica as _caustica_preimported");
        }
        catch (const std::exception& e)
        {
            donut::log::warning("PythonScripting: post-init script raised %s", e.what());
        }
    }

    m_initialized = true;
    donut::log::info("PythonScripting: embedded CPython %s ready", Py_GetVersion());
    return true;
}

void PythonScripting::QueueScriptFile(const std::filesystem::path& scriptPath)
{
    PendingScript ps;
    ps.isFile = true;
    ps.body   = scriptPath.string();
    ps.label  = scriptPath.filename().string();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.emplace_back(std::move(ps));
}

void PythonScripting::QueueScriptString(std::string code, std::string label)
{
    PendingScript ps;
    ps.isFile = false;
    ps.body   = std::move(code);
    ps.label  = std::move(label);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.emplace_back(std::move(ps));
}

bool PythonScripting::RunPendingLocked(const PendingScript& script)
{
    if (!m_initialized)
        return false;

    nb::gil_scoped_acquire gil;
    try
    {
        if (script.isFile)
        {
            FILE* fp = nullptr;
#ifdef _MSC_VER
            fopen_s(&fp, script.body.c_str(), "rb");
#else
            fp = std::fopen(script.body.c_str(), "rb");
#endif
            if (!fp)
            {
                donut::log::error("PythonScripting: cannot open script '%s'", script.body.c_str());
                return false;
            }
            int rc = PyRun_SimpleFileEx(fp, script.body.c_str(), 1 /*close file*/);
            if (rc != 0)
            {
                donut::log::error("PythonScripting: script '%s' raised an unhandled exception", script.label.c_str());
                return false;
            }
        }
        else
        {
            int rc = PyRun_SimpleString(script.body.c_str());
            if (rc != 0)
            {
                donut::log::error("PythonScripting: inline script '%s' raised an unhandled exception", script.label.c_str());
                return false;
            }
        }
    }
    catch (const std::exception& e)
    {
        donut::log::error("PythonScripting: %s threw a C++ exception: %s", script.label.c_str(), e.what());
        return false;
    }

    // Drain stdout/stderr capture buffers into m_outputLog.
    try
    {
        nb::object sys = nb::module_::import_("sys");
        for (const char* attr : { "_caustica_capture_stdout", "_caustica_capture_stderr" })
        {
            if (PyObject_HasAttrString(sys.ptr(), attr))
            {
                nb::object cap = sys.attr(attr);
                std::string chunk = nb::cast<std::string>(cap.attr("consume")());
                if (!chunk.empty())
                    m_outputLog += chunk;
            }
        }
    }
    catch (...) {}

    return true;
}

void PythonScripting::ProcessPendingScripts()
{
    if (!m_initialized)
        return;

    std::vector<PendingScript> local;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        local.swap(m_queue);
    }
    for (auto& ps : local)
        RunPendingLocked(ps);
}

bool PythonScripting::ExecuteImmediate(const std::string& code, const std::string& label)
{
    PendingScript ps;
    ps.isFile = false;
    ps.body   = code;
    ps.label  = label;
    return RunPendingLocked(ps);
}

std::string PythonScripting::ConsumeOutputLog()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string out;
    out.swap(m_outputLog);
    return out;
}

#else // RTXPT_WITH_PYTHON

#include <core/log.h>

PythonScripting::PythonScripting(Sample& app) : m_app(app) {}
PythonScripting::~PythonScripting() {}

bool PythonScripting::Initialize()
{
    donut::log::warning("PythonScripting: built without RTXPT_WITH_PYTHON, scripting unavailable");
    return false;
}
void        PythonScripting::QueueScriptFile  (const std::filesystem::path&)             {}
void        PythonScripting::QueueScriptString(std::string, std::string)                 {}
void        PythonScripting::ProcessPendingScripts()                                     {}
std::string PythonScripting::ConsumeOutputLog ()                                         { return {}; }
bool        PythonScripting::ExecuteImmediate (const std::string&, const std::string&)   { return false; }

#endif // RTXPT_WITH_PYTHON
