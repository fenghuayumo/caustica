// Shared bindings between the embedded Python (caustica.exe) and the Python
// extension module (caustica.pyd).  The actual NB_MODULE() definitions live
// in their respective hosts and only differ in how the running PathTracerApp is
// looked up:
//
//   - Embed     : module-level `app()` returns the singleton set by the
//                 PathTracerApp constructor (g_pythonPathTracerAppSingleton).
//   - Extension : the `Renderer` class owns its private PathTracerApp instance
//                 and the `app()` accessor returns its current renderer.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <nanobind/nanobind.h>

class PathTracerApp;

namespace caustica_py
{
    // Registers Material / Light / Settings / Sample type bindings into `m`.
    // Module-level free functions like `app()` / `settings()` are added by
    // the embed/extension entry points themselves.
    void RegisterCoreBindings(nanobind::module_& m);
}

// Shared singleton pointer used by the embedded mode.  Set by PythonScripting
// before Py_Initialize and consumed by PythonBindings_Embed.cpp.
extern PathTracerApp* g_pythonPathTracerAppSingleton;

#endif // CAUSTICA_WITH_PYTHON
