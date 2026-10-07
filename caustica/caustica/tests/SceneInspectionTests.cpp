#include <scene/SceneInspection.h>
#include <scene/SceneEcs.h>
#include <scene/ScenePoseAccess.h>

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void run()
{
    using namespace caustica;
    scene::SceneEntityWorld world;
    world.createEntity("Root");
    const auto parent = world.createEntity("Parent");
    const auto child = world.createEntity("Child", parent);
    world.world().emplace<scene::SceneAuthoringIdComponent>(parent, scene::SceneAuthoringIdComponent{"parent-id"});
    world.setTranslation(parent, math::double3(2, 0, 0));
    world.setTranslation(child, math::double3(0, 3, 0));
    world.rebuildPathsFromRoot();
    world.refreshHierarchy(scene::PreviousTransformPolicy::PreserveExisting);
    const auto snapshot = scene::inspectSceneEntities(world);
    check(snapshot["total"].asUInt64() == 2, "Root must not be counted");
    const auto& a = snapshot["entities"][0];
    const auto& b = snapshot["entities"][1];
    check(a["authored_id"] == "parent-id" && b["authored_id"].isNull(), "Authored and imported ids must be distinct");
    check(a["children_runtime_ids"][0] == b["runtime_id"] && b["parent_runtime_id"] == a["runtime_id"], "Broken hierarchy references");
    check(b["world_pose"]["translation"][0].asDouble() == 2 && b["world_pose"]["translation"][1].asDouble() == 3,
        "World pose was not composed");
    check(b["bounds"].isNull(), "Empty bounds must not emit infinities");
    check(scene::inspectSceneEntities(world, 1, 1)["entities"].size() == 1, "Pagination failed");
    check(scene::inspectSceneEntities(world, std::numeric_limits<size_t>::max(), 100)["entities"].empty(), "Offset overflow");
    check(scene::inspectSceneEntities(world, 0, 0)["entities"].empty(), "Zero limit failed");
    check(scene::inspectSceneEntities(world) == snapshot, "Query changed live state");
    world.world().emplace<scene::PrefabInstanceComponent>(parent, scene::PrefabInstanceComponent{"test.prefab.json", {}});
    world.world().emplace<scene::SceneAuthoringIdComponent>(child, scene::SceneAuthoringIdComponent{"internal-id"});
    const auto scoped = scene::inspectSceneEntities(world);
    check(scoped["entities"][1]["authored_scope_runtime_id"] == scoped["entities"][0]["runtime_id"],
        "Prefab-internal id must carry instance scope");
}
}

int main()
{
    try { run(); }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "SceneInspection test failed: %s\n", error.what());
        return 1;
    }
    std::puts("SceneInspection tests passed");
    return 0;
}
