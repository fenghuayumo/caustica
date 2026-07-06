// Shared bindings between the embedded Python (caustica.exe) and the Python
// extension module (caustica.pyd).  The actual NB_MODULE() definitions live
// in their respective hosts and only differ in how the running Sample is
// looked up:
//
//   - Embed     : module-level `app()` returns the singleton SceneEditor set
//                 by PythonScripting (g_pythonSceneEditorSingleton).
//   - Extension : Renderer.app returns the RenderSession-owned SceneSession.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <nanobind/nanobind.h>

namespace caustica::editor { class SceneEditor; }

namespace caustica_py
{
    // Registers Material / Light / Settings / Sample type bindings into `m`.
    // Module-level free functions like `app()` / `settings()` are added by
    // the embed/extension entry points themselves.
    void RegisterCoreBindings(nanobind::module_& m);
}

// Shared singleton pointer used by the embedded mode.  Set by PythonScripting
// before Py_Initialize and consumed by PythonBindings_Embed.cpp.
extern caustica::editor::SceneEditor* g_pythonSceneEditorSingleton;

#endif // CAUSTICA_WITH_PYTHON
