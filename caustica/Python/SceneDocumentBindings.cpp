#if CAUSTICA_WITH_PYTHON
#include "SceneDocumentBindings.h"

#include <scene/SceneDocument.h>
#include <core/json.h>
#include <nanobind/stl/string.h>

namespace nb = nanobind;
using caustica::scene::SceneDocument;

namespace
{
Json::Value fromPython(nb::handle value)
{
    const auto text = nb::cast<std::string>(nb::module_::import_("json").attr("dumps")(
        value, nb::arg("allow_nan") = false));
    Json::Value json;
    if (!caustica::json::fromString(text, json))
        throw nb::value_error("Invalid JSON value");
    return json;
}

nb::object toPython(const Json::Value& value)
{
    return nb::module_::import_("json").attr("loads")(caustica::json::toString(value));
}
}

void RegisterSceneDocumentBindings(nb::module_& module)
{
    nb::class_<SceneDocument>(module, "SceneDocument",
        "CPU-only authoring document with atomic batches, revision checks and undo. "
        "Save and load through EngineApp.set_scene explicitly to publish edits.")
        .def(nb::init<>())
        .def_static("from_dict", [](nb::dict value) { return SceneDocument(fromPython(value)); }, nb::arg("document"))
        .def_static("from_json", &SceneDocument::fromJson, nb::arg("source"))
        .def_static("load", [](const std::string& path) { return SceneDocument::load(path); }, nb::arg("path"))
        .def_static("capabilities", [] { return toPython(SceneDocument::capabilities()); })
        .def_prop_ro("revision", &SceneDocument::revision)
        .def_prop_ro("session_id", &SceneDocument::sessionId)
        .def_prop_ro("document", [](const SceneDocument& self) { return toPython(self.document()); })
        .def("to_json", &SceneDocument::toJson)
        .def("get_entity", [](const SceneDocument& self, const std::string& id) {
            return toPython(self.getEntity(id));
        }, nb::arg("id"))
        .def("query_entities", [](const SceneDocument& self, const std::string& component, size_t offset, size_t limit) {
            return toPython(self.queryEntities(component, offset, limit));
        }, nb::arg("component") = "", nb::arg("offset") = 0, nb::arg("limit") = 100)
        .def("execute", [](SceneDocument& self, nb::object request) { return toPython(self.execute(fromPython(request))); },
            nb::arg("request"))
        .def("save", [](const SceneDocument& self, const std::string& path) { return toPython(self.save(path)); },
            nb::arg("path"));
}
#endif
