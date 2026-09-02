// Embed-mode entry point: `import caustica` inside caustica.exe.

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"
#include "PythonEngineApp.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <engine/EngineApp.h>
#include <caustica/version.h>

#include <stdexcept>

namespace nb = nanobind;

NB_MODULE(caustica, m)
{
    m.doc() = "caustica embedded Python bindings (in-process scripting host).";
    m.attr("__version__") = CAUSTICA_VERSION_STRING;

    caustica_py::RegisterCoreBindings(m);

    auto engineClass = nb::class_<caustica_py::PyEngineApp>(m, "EngineApp",
        "Borrowed view of the running editor EngineApp. Methods match C++ in snake_case.");
    caustica_py::BindEngineApp(engineClass);

    auto requireEngine = []() -> caustica_py::PyEngineApp {
        caustica::EngineApp* engine = caustica_py::embedEngine();
        if (!engine)
            throw std::runtime_error("caustica: no EngineApp bound (embedded Python host not initialized?)");
        return caustica_py::PyEngineApp(engine);
    };

    m.def("engine", requireEngine, "Return the EngineApp bound by the embedded host.");
    m.def("settings", []() -> PathTracerSettings* {
            caustica::EngineApp* engine = caustica_py::embedEngine();
            if (!engine)
                throw std::runtime_error("caustica: no EngineApp bound");
            return &engine->settings();
        },
        nb::rv_policy::reference,
        "Shortcut for caustica.engine().settings.");

    m.attr("MODE") = "embed";
}

#endif // CAUSTICA_WITH_PYTHON
