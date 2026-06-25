// Embed-mode entry point: registers the `caustica` module that the embedded
// CPython interpreter (running inside caustica.exe) will see when scripts run
// `import caustica`.

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "SceneEditor.h"
#include <SampleUI.h>

#include <stdexcept>

namespace nb = nanobind;

namespace
{
    SceneEditor& RequireApp()
    {
        if (!g_pythonSceneEditorSingleton)
            throw std::runtime_error("caustica: no SceneEditor instance bound (renderer not initialized?)");
        return *g_pythonSceneEditorSingleton;
    }
}

NB_MODULE(caustica, m)
{
    m.doc() = "caustica embedded Python bindings (in-process scripting host).";

    caustica_py::RegisterCoreBindings(m);

    m.def("app", []() -> SceneEditor* { return &RequireApp(); },
          nb::rv_policy::reference,
          "Return the singleton SceneEditor renderer running in this caustica.exe.");

    m.def("settings", []() -> SampleUIData* { return &RequireApp().GetUIData(); },
          nb::rv_policy::reference,
          "Shortcut for caustica.app().settings.");

    m.attr("MODE") = "embed";
}

#endif // CAUSTICA_WITH_PYTHON
