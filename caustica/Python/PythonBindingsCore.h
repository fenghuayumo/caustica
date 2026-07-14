// Shared bindings between the embedded Python (caustica.exe) and the Python
// extension module (caustica.pyd).  The actual NB_MODULE() definitions live
// in their respective hosts and only differ in how the running Sample (App) is
// looked up:
//
//   - Embed     : module-level `app()` returns SceneEditor::app() from the
//                 singleton set by PythonScripting (g_pythonSceneEditorSingleton).
//   - Extension : Renderer.app returns EngineApp::app() from the RenderSession.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <nanobind/nanobind.h>

namespace caustica::editor { class SceneEditor; }

namespace caustica_py
{
    // Registers Material / SceneNode light props / settings / Sample type bindings into `m`.
    // Module-level free functions like `app()` / `settings()` are added by
    // the embed/extension entry points themselves.
    void RegisterCoreBindings(nanobind::module_& m);
}

// Shared singleton pointer used by the embedded mode.  Set by PythonScripting
// before Py_Initialize and consumed by PythonBindings_Embed.cpp.
extern caustica::editor::SceneEditor* g_pythonSceneEditorSingleton;

#endif // CAUSTICA_WITH_PYTHON
