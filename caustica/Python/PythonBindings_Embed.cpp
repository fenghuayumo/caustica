// Embed-mode entry point: registers the `caustica` module that the embedded
// CPython interpreter (running inside caustica.exe) will see when scripts run
// `import caustica`.

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "SceneEditor.h"
#include <engine/App.h>
#include <engine/SceneApi.h>

#include <stdexcept>

namespace nb = nanobind;
using caustica::App;
using caustica::editor::SceneEditor;

namespace
{
    SceneEditor& RequireSceneEditor()
    {
        if (!g_pythonSceneEditorSingleton)
            throw std::runtime_error("caustica: no SceneEditor instance bound (renderer not initialized?)");
        return *g_pythonSceneEditorSingleton;
    }

    App& RequireApp()
    {
        SceneEditor& editor = RequireSceneEditor();
        App* app = editor.app();
        if (!app)
            throw std::runtime_error("caustica: SceneEditor has no App bound");
        return *app;
    }
}

NB_MODULE(caustica, m)
{
    m.doc() = "caustica embedded Python bindings (in-process scripting host).";

    caustica_py::RegisterCoreBindings(m);

    m.def("app", []() -> App* { return &RequireApp(); },
          nb::rv_policy::reference,
          "Return the App owned by the SceneEditor running in this caustica.exe.");

    m.def("settings", []() -> PathTracerSettings* {
            return caustica::settings(RequireApp());
        },
          nb::rv_policy::reference,
          "Shortcut for caustica.app().settings.");

    m.attr("MODE") = "embed";
}

#endif // CAUSTICA_WITH_PYTHON
