// Embed-mode entry point: registers the `caustica` module that the embedded
// CPython interpreter (running inside caustica.exe) will see when scripts run
// `import caustica`.

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <caustica/version.h>

#include <stdexcept>

namespace nb = nanobind;
using caustica::App;

namespace
{
    App& RequireApp()
    {
        caustica::App* app = caustica_py::embedApp();
        if (!app)
            throw std::runtime_error("caustica: no App instance bound (embedded Python host not initialized?)");
        return *app;
    }
}

NB_MODULE(caustica, m)
{
    m.doc() = "caustica embedded Python bindings (in-process scripting host).";
    m.attr("__version__") = CAUSTICA_VERSION_STRING;

    caustica_py::RegisterCoreBindings(m);

    m.def("app", []() -> App* { return &RequireApp(); },
          nb::rv_policy::reference,
          "Return the App bound by the embedded Python host in this caustica.exe.");

    m.def("settings", []() -> PathTracerSettings* {
            return caustica::settings(RequireApp());
        },
          nb::rv_policy::reference,
          "Shortcut for caustica.app().settings.");

    m.attr("MODE") = "embed";
}

#endif // CAUSTICA_WITH_PYTHON
