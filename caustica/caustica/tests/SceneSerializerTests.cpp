#include <scene/SceneSerializer.h>
#include <scene/SceneEcs.h>
#include <scene/SceneCameraAccess.h>
#include <scene/ScenePoseAccess.h>
#include <scene/SceneComponentBuilders.h>
#include <scene/SceneObjects.h>
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
        const Json::Value node = parse(R"({
            "enabled": true,
            "color": [0.8, 0.6, 0.4],
            "intensity": 15.0,
            "width": 2.5,
            "height": 1.25
        })");
        auto component = caustica::scene::makeLightComponentFromJson("RectLight", node);
        const auto* rect = component ? std::get_if<caustica::scene::RectLightComponent>(&*component) : nullptr;
        passed &= expect(rect && std::abs(rect->intensity - 15.f) < 1e-5f,
            "RectLight intensity was not parsed");
        passed &= expect(rect && std::abs(rect->width - 2.5f) < 1e-5f
                && std::abs(rect->height - 1.25f) < 1e-5f,
            "RectLight dimensions were not parsed");
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

    {
        caustica::SceneSettings settings;
        settings.load(parse(R"({"realtimeMode": true})"));
        passed &= expect(settings.realtimeMode && *settings.realtimeMode, "realtimeMode not loaded");
        passed &= expect(!settings.environment.has_value(), "missing environment key should not invent look settings");
        passed &= expect(!settings.gaussianSplat.has_value(), "missing gaussianSplat key should not invent look settings");
        passed &= expect(settings.hiddenEntities.empty(), "missing hiddenEntities should stay empty");

        settings.load(parse(R"({
            "environment": { "intensity": 2.5 },
            "gaussianSplat": { "brightness": 1.5 },
            "hiddenEntities": ["/Root/Hidden"]
        })"));
        passed &= expect(
            settings.environment && settings.environment->intensity
                && std::abs(*settings.environment->intensity - 2.5f) < 1e-5f,
            "environment.intensity not loaded");
        passed &= expect(
            settings.environment && !settings.environment->tintColor.has_value(),
            "environment load applied a field that was not in JSON");
        passed &= expect(
            settings.gaussianSplat && settings.gaussianSplat->brightness
                && std::abs(*settings.gaussianSplat->brightness - 1.5f) < 1e-5f,
            "gaussianSplat.brightness not loaded");
        passed &= expect(
            settings.hiddenEntities.size() == 1 && settings.hiddenEntities[0] == "/Root/Hidden",
            "hiddenEntities not loaded");

        Json::Value node(Json::objectValue);
        node["realtimeMode"] = true;
        settings.writeLook(node);
        passed &= expect(node["realtimeMode"].asBool(), "writeLook replaced unrelated settings");
        passed &= expect(node["environment"]["intensity"].asFloat() > 2.f, "writeLook dropped intensity");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity sun = world.createEntity("Sun", root);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            sun, caustica::scene::SceneAuthoringIdComponent{ "sun" });
        caustica::scene::DirectionalLightComponent light;
        light.color = dm::float3(1.f, 0.5f, 0.25f);
        light.irradiance = 4.f;
        light.angularSize = 1.5f;
        world.world().emplace<caustica::scene::DirectionalLightComponent>(sun, light);

        caustica::scene::CameraComponent camera;
        caustica::scene::PerspectiveCameraData pers;
        pers.verticalFov = 0.9f;
        pers.zNear = 0.05f;
        pers.zFar = 500.f;
        camera.data = pers;
        const caustica::ecs::Entity cam = world.createEntity("Camera", root);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cam, caustica::scene::SceneAuthoringIdComponent{ "cam" });
        world.world().emplace<caustica::scene::CameraComponent>(cam, camera);

        Json::Value entities = parse(R"([
            {"id":"sun","name":"Sun","components":{"DirectionalLight":{"irradiance":1.0}}},
            {"id":"cam","name":"Camera","components":{"PerspectiveCameraEx":{"verticalFov":0.7}}}
        ])");
        caustica::scene::patchEntityTransforms(entities, world);
        passed &= expect(
            std::abs(entities[0]["components"]["DirectionalLight"]["irradiance"].asFloat() - 4.f) < 1e-5f,
            "save patch did not write DirectionalLight.irradiance");
        passed &= expect(
            entities[0]["components"]["DirectionalLight"]["color"].isArray(),
            "save patch did not write DirectionalLight.color");
        passed &= expect(
            std::abs(entities[1]["components"]["PerspectiveCameraEx"]["verticalFov"].asFloat() - 0.9f) < 1e-5f,
            "save patch did not write PerspectiveCameraEx.verticalFov");
        passed &= expect(
            std::abs(entities[1]["components"]["PerspectiveCameraEx"]["zNear"].asFloat() - 0.05f) < 1e-5f,
            "save patch did not write PerspectiveCameraEx.zNear");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity cam = world.createEntity("Camera", root);
        caustica::scene::CameraComponent camera;
        caustica::scene::PerspectiveCameraData pers;
        pers.verticalFov = 0.7f;
        pers.zNear = 0.1f;
        pers.intrinsics = caustica::scene::CameraIntrinsics{ 800.f, 800.f, 640.f, 360.f, 1280.f, 720.f };
        camera.data = pers;
        world.world().emplace<caustica::scene::CameraComponent>(cam, camera);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cam, caustica::scene::SceneAuthoringIdComponent{ "cam" });

        Json::Value entities = parse(R"([
            {"id":"cam","name":"Camera","components":{"PerspectiveCameraEx":{"verticalFov":0.7}}}
        ])");
        caustica::scene::patchEntityTransforms(entities, world);
        passed &= expect(
            std::abs(entities[0]["components"]["PerspectiveCameraEx"]["fx"].asFloat() - 800.f) < 1e-5f,
            "save patch did not write PerspectiveCameraEx.fx");
        passed &= expect(
            std::abs(entities[0]["components"]["PerspectiveCameraEx"]["height"].asFloat() - 720.f) < 1e-5f,
            "save patch did not write PerspectiveCameraEx.height");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity cam = world.createEntity("Camera", root);
        caustica::scene::CameraComponent camera;
        camera.data = caustica::scene::PerspectiveCameraData{};
        world.world().emplace<caustica::scene::CameraComponent>(cam, camera);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cam, caustica::scene::SceneAuthoringIdComponent{ "cam" });

        Json::Value entities = parse(R"([
            {"id":"cam","name":"Camera","components":{
                "PerspectiveCameraEx": {
                    "verticalFov":0.7,
                    "fx":800,"fy":800,"cx":640,"cy":360,"width":1280,"height":720
                }
            }}
        ])");
        caustica::scene::patchEntityTransforms(entities, world);
        const Json::Value& saved = entities[0]["components"]["PerspectiveCameraEx"];
        passed &= expect(!saved.isMember("fx") && !saved.isMember("fy")
                && !saved.isMember("cx") && !saved.isMember("cy")
                && !saved.isMember("width") && !saved.isMember("height"),
            "cleared camera intrinsics were not removed from the save patch");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity cam = world.createEntity("Camera", root);
        caustica::scene::CameraComponent camera;
        camera.data = caustica::scene::PerspectiveCameraData{};
        world.world().emplace<caustica::scene::CameraComponent>(cam, camera);
        world.refreshHierarchy();
        passed &= expect(
            caustica::scene::setCameraWorldLookTo(
                world, cam, dm::float3(0.f, 1.5f, 4.f), dm::float3(0.f, 0.f, -1.f), dm::float3(0.f, 1.f, 0.f)),
            "setCameraWorldLookTo failed");
        caustica::scene::CameraWorldLookTo look;
        passed &= expect(
            caustica::scene::tryGetCameraWorldLookTo(world, cam, look),
            "tryGetCameraWorldLookTo failed");
        passed &= expect(std::abs(look.position.y - 1.5f) < 1e-4f, "look_to did not keep world position");
        passed &= expect(std::abs(look.direction.z + 1.f) < 1e-3f, "look_to did not keep look direction");
        passed &= expect(std::abs(look.up.y - 1.f) < 1e-3f, "look_to did not keep up");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity child = world.createEntity("Child", root);
        world.setTranslation(root, dm::double3(4.0, 1.0, -2.0));
        world.setRotation(root, dm::rotationQuat(dm::double3(0.0, dm::PI_d * 0.5, 0.0)));
        world.refreshHierarchy();

        caustica::scene::EntityPose desired;
        desired.position = dm::double3(7.0, 2.0, -3.0);
        desired.rotation = dm::dquat::identity();
        desired.scaling = dm::double3(1.0);
        passed &= expect(
            caustica::scene::setEntityWorldPose(world, child, desired),
            "setEntityWorldPose failed under a rotated parent");
        world.refreshHierarchy();
        caustica::scene::EntityPose actual;
        passed &= expect(
            caustica::scene::getEntityWorldPose(world, child, actual),
            "getEntityWorldPose failed after a rotated-parent write");
        passed &= expect(
            std::abs(actual.position.x - desired.position.x) < 1e-4
                && std::abs(actual.position.y - desired.position.y) < 1e-4
                && std::abs(actual.position.z - desired.position.z) < 1e-4,
            "world pose write used the wrong parent multiplication order");
    }

    {
        Json::Value src = parse(R"({
            "verticalFov": 0.7,
            "zNear": 0.1,
            "fx": 900, "fy": 900, "cx": 640, "cy": 360, "width": 1280, "height": 720
        })");
        auto camera = caustica::scene::makeCameraComponentFromJson("PerspectiveCameraEx", src);
        passed &= expect(camera.has_value(), "failed to parse PerspectiveCameraEx with intrinsics");
        const auto* pers = camera ? caustica::scene::tryGetPerspectiveCameraData(*camera) : nullptr;
        passed &= expect(
            pers && pers->intrinsics && std::abs(pers->intrinsics->fx - 900.f) < 1e-5f,
            "JSON did not load camera fx");
        passed &= expect(
            pers && pers->intrinsics && std::abs(pers->intrinsics->height - 720.f) < 1e-5f,
            "JSON did not load camera height");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity visible = world.createEntity("VisibleMesh", root);
        const caustica::ecs::Entity hidden = world.createEntity("HiddenMesh", root);
        world.world().emplace<caustica::scene::MeshInstanceComponent>(
            visible, caustica::scene::MeshInstanceComponent{});
        world.world().emplace<caustica::scene::MeshInstanceComponent>(
            hidden, caustica::scene::MeshInstanceComponent{});
        world.rebuildPathsFromRoot();

        const std::string hiddenPath = world.getEntityPath(hidden).generic_string();
        passed &= expect(
            caustica::ecs::isValid(world.entityForPath(hiddenPath)),
            "entityForPath could not resolve a rebuilt hidden mesh path");

        caustica::scene::applyHiddenEntities(world, { hiddenPath });
        const auto* visibleMesh = world.world().tryGet<caustica::scene::MeshInstanceComponent>(visible);
        const auto* hiddenMesh = world.world().tryGet<caustica::scene::MeshInstanceComponent>(hidden);
        passed &= expect(visibleMesh && visibleMesh->enabled,
            "hiddenEntities hid a mesh that was not listed");
        passed &= expect(hiddenMesh && !hiddenMesh->enabled,
            "hiddenEntities did not hide the listed mesh");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity sun = world.createEntity("Sun", root);
        caustica::scene::DirectionalLightComponent light;
        light.irradiance = 7.f;
        light.color = dm::float3(0.1f, 0.2f, 0.3f);
        world.world().emplace<caustica::scene::DirectionalLightComponent>(sun, light);
        world.rebuildPathsFromRoot();

        Json::Value document(Json::objectValue);
        caustica::scene::patchEntityOverrides(document, world);
        passed &= expect(
            document["entityOverrides"].isArray() && document["entityOverrides"].size() == 1,
            "unauthored light was not written to entityOverrides");

        caustica::scene::SceneEntityWorld loaded;
        const caustica::ecs::Entity loadedRoot = loaded.createEntity("Root");
        const caustica::ecs::Entity loadedSun = loaded.createEntity("Sun", loadedRoot);
        loaded.world().emplace<caustica::scene::DirectionalLightComponent>(
            loadedSun, caustica::scene::DirectionalLightComponent{});
        loaded.rebuildPathsFromRoot();
        caustica::scene::applyEntityOverrides(loaded, document["entityOverrides"]);
        const auto* loadedLight = loaded.world().tryGet<caustica::scene::DirectionalLightComponent>(loadedSun);
        passed &= expect(
            loadedLight && std::abs(loadedLight->irradiance - 7.f) < 1e-5f,
            "entityOverrides did not apply DirectionalLight.irradiance");
    }

    {
        caustica::scene::SceneEntityWorld world;
        const caustica::ecs::Entity root = world.createEntity("Root");
        const caustica::ecs::Entity cube = world.createEntity("Cube", root);
        world.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cube, caustica::scene::SceneAuthoringIdComponent{ "Cube" });
        world.world().emplace<caustica::scene::PrefabInstanceComponent>(
            cube, caustica::scene::PrefabInstanceComponent{ "builtin:cube", {} });
        world.setTranslation(cube, dm::double3(1.0, 0.5, 2.0));

        Json::Value document(Json::objectValue);
        caustica::scene::upsertAuthoredEntityNode(document, world, cube);
        passed &= expect(
            document["entities"].isArray() && document["entities"].size() == 1,
            "upsert did not append an authored entity");
        passed &= expect(
            document["entities"][0]["components"]["PrefabInstance"]["source"].asString() == "builtin:cube",
            "upsert did not write PrefabInstance.source");

        world.setTranslation(cube, dm::double3(3.0, 0.5, 2.0));
        caustica::scene::syncAuthoredEntitiesToDocument(document, world);
        passed &= expect(document["entities"].size() == 1, "sync duplicated the authored entity");

        caustica::scene::removeAuthoredEntityNode(document, "Cube");
        passed &= expect(
            document["entities"].empty(),
            "removeAuthoredEntityNode did not drop the entity");
    }

    return passed ? 0 : 1;
}
