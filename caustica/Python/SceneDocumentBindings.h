#pragma once

#if CAUSTICA_WITH_PYTHON
#include <nanobind/nanobind.h>
void RegisterSceneDocumentBindings(nanobind::module_& module);
#endif
