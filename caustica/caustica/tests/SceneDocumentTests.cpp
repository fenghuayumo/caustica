#include <scene/SceneDocument.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

using caustica::scene::SceneDocument;

namespace
{
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Json::Value request(const std::string& operations, uint64_t revision)
{
    Json::Value root = SceneDocument::fromJson("{\"entities\":[],\"operations\":" + operations + "}").document();
    root.removeMember("entities");
    root["expected_revision"] = Json::UInt64(revision);
    root["request_id"] = "test";
    return root;
}

void success(const Json::Value& value)
{
    if (value["status"].asString() != "completed")
        throw std::runtime_error(value.toStyledString());
}

void failure(const Json::Value& value, const char* code)
{
    check(value["status"] == "failed" && value["error"]["code"] == code, "Wrong failure code");
}

void run()
{
    SceneDocument scene;
    const auto setup = request(R"([
        {"op":"add_entity","entity":{"id":"room"}},
        {"op":"add_entity","entity":{"id":"cube","parent":"room","components":{
            "PrefabInstance":{"source":"builtin:cube"},"Transform":{"translation":[0,1,0]},
            "MaterialOverride":{"source":"materials/red.material.json"},"Extension":{"value":1}}}},
        {"op":"add_entity","entity":{"id":"sun","components":{"DirectionalLight":{"irradiance":3}}}},
        {"op":"add_entity","entity":{"id":"camera","components":{"PerspectiveCameraEx":{"verticalFov":0.7}}}},
        {"op":"patch_settings","patch":{"realtimeMode":true}}
    ])", 0);
    const auto first = scene.execute(setup);
    success(first);
    check(first["request_id"] == "test" && first["created_ids"].size() == 4 && scene.revision() == 1,
        "Batch must be a single revision with correlated results");
    const auto initial = scene.document();
    failure(scene.execute(setup), "revision_conflict");
    auto wrongSession = request(R"([{"op":"patch_settings","patch":{}}])", 1);
    wrongSession["expected_session_id"] = "other-document";
    failure(scene.execute(wrongSession), "session_conflict");
    auto invalid = request(R"([
        {"op":"patch_entity","id":"cube","patch":{"name":"should rollback"}},
        {"op":"patch_entity","id":"sun","patch":{"parent":"missing"}}
    ])", 1);
    failure(scene.execute(invalid), "entity_not_found");
    check(scene.document() == initial && scene.revision() == 1, "Failed batch leaked changes");

    success(scene.execute(request(R"([{"op":"patch_entity","id":"cube","patch":{
        "name":"Renamed", "components":{"Transform":{"scale":[2,2,2]},"Extension":null}}}])", 1)));
    const auto cube = scene.getEntity("cube");
    check(cube["id"] == "cube" && cube["components"]["Transform"]["translation"].size() == 3,
        "Rename/merge lost id or sibling fields");
    check(!cube["components"].isMember("Extension"), "Merge patch null must remove fields");
    failure(scene.execute(request(R"([{"op":"patch_entity","id":"cube","patch":{"id":"changed"}}])", 2)), "invalid_argument");
    failure(scene.execute(request(R"([{"op":"patch_entity","id":"room","patch":{"parent":"cube"}}])", 2)), "hierarchy_cycle");
    failure(scene.execute(request(R"([{"op":"remove_entity","id":"room"}])", 2)), "entity_has_children");
    failure(scene.execute(request(R"([{"op":"patch_entity","id":"cube","patch":{"components":{"Transform":{"rotation":[0,0,0,0]}}}}])", 2)), "invalid_argument");
    failure(scene.execute(request(R"([{"op":"patch_entity","id":"camera","patch":{"components":{"PerspectiveCameraEx":{"verticalFov":4}}}}])", 2)), "invalid_argument");
    failure(scene.execute(request(R"([{"op":"patch_entity","id":"sun","patch":{"components":{"DirectionalLight":{"irradiance":-1}}}}])", 2)), "invalid_argument");
    auto nonfinite = request(R"([{"op":"patch_settings","patch":{"test":0}}])", 2);
    nonfinite["operations"][0]["patch"]["test"] = std::numeric_limits<double>::quiet_NaN();
    failure(scene.execute(nonfinite), "invalid_argument");
    check(scene.revision() == 2, "Rejected edits advanced revision");

    success(scene.execute(request(R"([{"op":"undo"}])", 2)));
    check(scene.document() == initial && scene.revision() == 3, "Undo must restore contents and advance revision");
    success(scene.execute(request(R"([{"op":"redo"}])", 3)));
    check(scene.getEntity("cube")["name"] == "Renamed" && scene.revision() == 4, "Redo failed");
    success(scene.execute(request(R"([{"op":"remove_entity","id":"room","cascade":true}])", 4)));
    check(scene.getEntity("cube").isNull() && scene.getEntity("room").isNull(), "Cascade failed");
    success(scene.execute(request(R"([{"op":"undo"}])", 5)));
    const auto beforeNoop = scene.revision();
    success(scene.execute(request(R"([{"op":"patch_entity","id":"cube","patch":{}}])", beforeNoop)));
    check(scene.revision() == beforeNoop, "No-op advanced revision");
    auto page = scene.queryEntities("PrefabInstance", 0, 1);
    check(page["total"].asUInt64() == 1 && page["entities"].size() == 1, "Filtered pagination failed");
    check(scene.queryEntities("", 999, 100)["entities"].empty(), "Out-of-range page failed");
    check(scene.queryEntities("", 0, 0)["entities"].empty(), "Zero-limit page failed");

    success(scene.execute(request(R"([{"op":"add_entity","entity":{"name":"Generated"}}])", scene.revision())));
    failure(scene.execute(request(R"([{"op":"redo"}])", scene.revision())), "history_empty");
    const auto temp = std::filesystem::temp_directory_path() / ("caustica-document-" + scene.sessionId() + ".scene.json");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{temp};
    success(scene.save(temp));
    auto loaded = SceneDocument::load(temp);
    check(loaded.document() == scene.document(), "Save/reload lost data or ids");
    check(loaded.revision() == 0 && loaded.sessionId() != scene.sessionId(), "Loaded session identity is not independent");
    success(scene.save(temp)); // Replace an existing file atomically.
    failure(scene.save(temp / "missing" / "scene.json"), "io_error");
    check(SceneDocument::load(temp).document() == scene.document(), "Failed save damaged existing document");
    failure(scene.execute(Json::Value(42)), "invalid_argument");
    failure(scene.execute(request(R"([{"op":"unknown"}])", scene.revision())), "unsupported_operation");
    failure(scene.execute(request(R"([{"op":"add_entity","entity":{"id":"cube"}}])", scene.revision())), "duplicate_id");

    for (const char* text : {R"({"base":"x","entities":[]})", R"({"entities":[{"id":"a"},{"id":"a"}]})",
             R"({"entities":[{"id":"a","parent":"missing"}]})", R"({"entities":[],"entities":[]})"})
    {
        bool rejected = false;
        try { (void)SceneDocument::fromJson(text); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid initial document accepted");
    }

    SceneDocument bounded(SceneDocument::emptyDocument(), 1);
    success(bounded.execute(request(R"([{"op":"add_entity","entity":{"id":"a"}}])", 0)));
    success(bounded.execute(request(R"([{"op":"add_entity","entity":{"id":"b"}}])", 1)));
    success(bounded.execute(request(R"([{"op":"undo"}])", 2)));
    failure(bounded.execute(request(R"([{"op":"undo"}])", 3)), "history_empty");
}
}

int main()
{
    try { run(); }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "SceneDocument test failed: %s\n", error.what());
        return 1;
    }
    std::puts("SceneDocument tests passed");
    return 0;
}
