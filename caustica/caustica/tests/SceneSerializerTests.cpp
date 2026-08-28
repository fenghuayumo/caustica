#include <scene/SceneSerializer.h>
#include <scene/SceneEcs.h>
#include <core/json.h>

#include <cstdio>
#include <string>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "SceneSerializer test failed: %s\n", message);
    return false;
}

Json::Value parse(const std::string& text)
{
    Json::Value root;
    if (!caustica::json::fromString(text, root))
        return Json::Value();
    return root;
}
}

int main()
{
    bool passed = true;

    {
        passed &= expect(
            caustica::scene::canonicalizeEnvSource("procedural:sky") == "==PROCEDURAL_SKY==",
            "procedural:sky did not map to the engine sky sentinel");
        passed &= expect(
            caustica::scene::canonicalizeEnvSource("env/foo.dds") == "env/foo.dds",
            "pack-relative env path was rewritten");
    }

    {
        Json::Value base = parse(R"({
            "settings": { "realtimeMode": true, "enableAnimations": true },
            "entities": [
                {
                    "id": "sky",
                    "name": "Sky",
                    "components": {
                        "EnvironmentLight": {
                            "radianceScale": [1, 1, 1],
                            "source": "env/simons_town_rocks_4k_cube_bc6u.dds"
                        }
                    }
                }
            ]
        })");
        Json::Value overlay = parse(R"({
            "base": "scenes/kitchen/kitchen.scene.json",
            "overrides": [
                { "id": "sky", "components": { "EnvironmentLight": { "source": "procedural:sky" } } }
            ]
        })");

        const Json::Value merged = caustica::scene::mergeSceneOverlay(base, overlay);
        passed &= expect(merged["settings"]["realtimeMode"].asBool(), "overlay dropped base settings");
        passed &= expect(
            merged["entities"][0]["components"]["EnvironmentLight"]["source"].asString() == "procedural:sky",
            "overlay did not replace EnvironmentLight.source");
        passed &= expect(
            merged["entities"][0]["components"]["EnvironmentLight"]["radianceScale"].isArray(),
            "overlay replaced the whole EnvironmentLight instead of patching source");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity cube = world.createEntity("Cube", root);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cube, caustica::scene::SceneAuthoringIdComponent{ "cube" });

        Json::Value transform = parse(R"({"translation":[0.0, 0.5, 0.0], "scale": 2.0})");
        caustica::scene::applyAuthoringTransform(world, cube, transform);
        const auto* local = world.world().tryGet<caustica::scene::LocalTransformComponent>(cube);
        passed &= expect(local && local->hasLocalTransform, "transform was not applied");
        passed &= expect(local && local->translation.y == 0.5, "translation.y mismatch");
        passed &= expect(local && local->scaling.x == 2.0 && local->scaling.y == 2.0,
            "scalar scale did not expand to xyz");

        Json::Value entities = parse(R"([{"id":"cube","name":"Cube","components":{}}])");
        caustica::scene::patchEntityTransforms(entities, world);
        passed &= expect(
            entities[0]["components"]["Transform"]["translation"].isArray(),
            "save patch did not write Transform.translation");
    }

    return passed ? 0 : 1;
}
