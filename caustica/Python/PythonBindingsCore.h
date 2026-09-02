// Shared bindings between the embedded Python (caustica.exe) and the Python
// extension module (caustica.pyd).  The actual NB_MODULE() definitions live
// in their respective hosts and only differ in how the running EngineApp is
// looked up:
//
//   - Embed     : module-level `engine()` returns the EngineApp bound by PythonScripting.
//   - Extension : `EngineApp.create(...)` owns a RenderSession / EngineApp.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include "PythonEngineApp.h"

#include <nanobind/nanobind.h>

namespace caustica { class EngineApp; }

namespace caustica_py
{
    void RegisterCoreBindings(nanobind::module_& m);
    void BindEngineApp(nanobind::class_<PyEngineApp>& cls);

    void setEmbedEngine(caustica::EngineApp* engine);
    [[nodiscard]] caustica::EngineApp* embedEngine();
}

#endif // CAUSTICA_WITH_PYTHON
