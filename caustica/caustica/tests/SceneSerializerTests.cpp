#include <scene/SceneSerializer.h>
#include <scene/SceneEcs.h>
#include <core/json.h>
#include <core/path_utils.h>

#include <cmath>
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

        Json::Value xyzRotation = parse(
            R"({"rotation":[-0.7071068, 0.0, 0.7071068]})");
        caustica::scene::applyAuthoringTransform(world, cube, xyzRotation);
        const auto* rotated = world.world().tryGet<caustica::scene::LocalTransformComponent>(cube);
        passed &= expect(rotated && rotated->hasLocalTransform, "xyz rotation was not applied");
        passed &= expect(
            rotated && std::abs(rotated->rotation.w - 1.0) < 1e-6,
            "3-element rotation should default w to 1 like Donut");

        Json::Value entities = parse(R"([{"id":"cube","name":"Cube","components":{}}])");
        caustica::scene::patchEntityTransforms(entities, world);
        passed &= expect(
            entities[0]["components"]["Transform"]["translation"].isArray(),
            "save patch did not write Transform.translation");
    }

    {
        passed &= expect(caustica::isPrefabAssetPath("prefabs/orb.prefab.json"),
            "prefab path not recognized");
        passed &= expect(caustica::isPrefabAssetPath("PREFAB/CUBE.PREFAB.JSON"),
            "prefab path matching should ignore case");
        passed &= expect(!caustica::isPrefabAssetPath("models/kitchen/kitchen.gltf"),
            "gltf was treated as a prefab");
        passed &= expect(caustica::isMaterialAssetPath("materials/foo.mat.json"),
            ".mat.json path not recognized");
        passed &= expect(caustica::isMaterialAssetPath("materials/foo.material.json"),
            ".material.json path not recognized");
    }

    {
        Json::Value base = parse(R"({
            "entities": [{
                "id": "kitchen",
                "components": {
                    "PrefabInstance": {
                        "source": "models/kitchen/kitchen.gltf",
                        "materials": { "Glass": "materials/kitchen.Glass.material.json" }
                    }
                }
            }]
        })");
        Json::Value overlay = parse(R"({
            "overrides": [{
                "id": "kitchen",
                "components": {
                    "PrefabInstance": {
                        "materials": { "Floor_MDL": "materials/kitchen.Floor_MDL.material.json" }
                    }
                }
            }]
        })");
        const Json::Value merged = caustica::scene::mergeSceneOverlay(base, overlay);
        passed &= expect(
            merged["entities"][0]["components"]["PrefabInstance"]["source"].asString()
                == "models/kitchen/kitchen.gltf",
            "material overlay dropped PrefabInstance.source");
        passed &= expect(
            merged["entities"][0]["components"]["PrefabInstance"]["materials"]["Glass"].asString()
                == "materials/kitchen.Glass.material.json",
            "material overlay dropped existing slot");
        passed &= expect(
            merged["entities"][0]["components"]["PrefabInstance"]["materials"]["Floor_MDL"].asString()
                == "materials/kitchen.Floor_MDL.material.json",
            "material overlay did not add the new slot");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity kitchen = world.createEntity("Kitchen", root);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            kitchen, caustica::scene::SceneAuthoringIdComponent{ "kitchen" });
        world.world().emplace<caustica::scene::PrefabInstanceComponent>(
            kitchen,
            caustica::scene::PrefabInstanceComponent{
                "models/kitchen/kitchen.gltf",
                { { "Glass", "materials/kitchen.Glass.material.json" } } });

        Json::Value entities = parse(R"([{
            "id":"kitchen",
            "name":"Kitchen",
            "components": { "PrefabInstance": { "source": "models/kitchen/kitchen.gltf" } }
        }])");
        caustica::scene::patchEntityTransforms(entities, world);
        passed &= expect(
            entities[0]["components"]["PrefabInstance"]["source"].asString()
                == "models/kitchen/kitchen.gltf",
            "save patch dropped PrefabInstance.source");
        passed &= expect(
            entities[0]["components"]["PrefabInstance"]["materials"]["Glass"].asString()
                == "materials/kitchen.Glass.material.json",
            "save patch did not write PrefabInstance.materials");
    }

    {
        Json::Value base = parse(R"({
            "entities": [{
                "id": "prop",
                "components": {
                    "MaterialOverride": {
                        "source": "materials/shared.Default.material.json",
                        "slots": { "Glass": "materials/kitchen.Glass.material.json" }
                    }
                }
            }]
        })");
        Json::Value overlay = parse(R"({
            "overrides": [{
                "id": "prop",
                "components": {
                    "MaterialOverride": {
                        "slots": { "Floor_MDL": "materials/kitchen.Floor_MDL.material.json" }
                    }
                }
            }]
        })");
        const Json::Value merged = caustica::scene::mergeSceneOverlay(base, overlay);
        passed &= expect(
            merged["entities"][0]["components"]["MaterialOverride"]["source"].asString()
                == "materials/shared.Default.material.json",
            "MaterialOverride overlay dropped source");
        passed &= expect(
            merged["entities"][0]["components"]["MaterialOverride"]["slots"]["Glass"].asString()
                == "materials/kitchen.Glass.material.json",
            "MaterialOverride overlay dropped existing slot");
        passed &= expect(
            merged["entities"][0]["components"]["MaterialOverride"]["slots"]["Floor_MDL"].asString()
                == "materials/kitchen.Floor_MDL.material.json",
            "MaterialOverride overlay did not add the new slot");
    }

    return passed ? 0 : 1;
}
