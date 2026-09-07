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

#include <engine/SensorApi.h>
#include <nanobind/nanobind.h>

namespace caustica { class EngineApp; }

namespace caustica_py
{
    nanobind::object sensorRgbNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorDepthNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorNormalNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorInstanceIdNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorSemanticIdNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorMotionVectorNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorDiffuseNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorRoughnessNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorSpecularNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorMetallicNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorThroughputNumpy(const caustica::SensorOutput& output);
    nanobind::object sensorGuideDiffuseNumpy(const caustica::SensorOutput& output);

    void RegisterCoreBindings(nanobind::module_& m);
    void BindEngineApp(nanobind::class_<PyEngineApp>& cls);

    void setEmbedEngine(caustica::EngineApp* engine);
    [[nodiscard]] caustica::EngineApp* embedEngine();
}

#endif // CAUSTICA_WITH_PYTHON
