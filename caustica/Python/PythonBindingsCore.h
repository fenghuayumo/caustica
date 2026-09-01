// Shared bindings between the embedded Python (caustica.exe) and the Python
// extension module (caustica.pyd).  The actual NB_MODULE() definitions live
// in their respective hosts and only differ in how the running App is
// looked up:
//
//   - Embed     : module-level `app()` returns the App* set by PythonScripting
//                 (g_pythonEmbedApp).
//   - Extension : Renderer.app returns EngineApp::app() from the RenderSession.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <nanobind/nanobind.h>

namespace caustica { class App; }

namespace caustica_py
{
    // Registers Material / SceneEntity / Scene / settings / Sample / ScenePrefab bindings.
    // Sample wraps engine CameraApi / SceneSpawn / MeshDeformApi / RenderSessionApi.
    // Module-level free functions like `app()` / `settings()` are added by
    // the embed/extension entry points themselves.
    void RegisterCoreBindings(nanobind::module_& m);
}

// Embed-mode App pointer. Set by the editor PythonScripting host before
// Py_Initialize; stays null in extension mode.
extern caustica::App* g_pythonEmbedApp;

#endif // CAUSTICA_WITH_PYTHON
