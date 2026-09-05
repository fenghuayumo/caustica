#include <scene/SceneSerializer.h>

#include <scene/Scene.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneComponentBuilders.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneObjects.h>
#include <core/json.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <math/math.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

using namespace caustica::math;

namespace caustica::scene
{
namespace
{

void MergeObjectFields(Json::Value& target, const Json::Value& overlay)
{
    if (!overlay.isObject())
        return;
    if (!target.isObject())
        target = Json::Value(Json::objectValue);

    const auto names = overlay.getMemberNames();
    for (const auto& name : names)
    {
        if (target[name].isObject() && overlay[name].isObject())
            MergeObjectFields(target[name], overlay[name]);
        else
            target[name] = overlay[name];
    }
}

Json::Value* FindEntityById(Json::Value& entities, const std::string& id)
{
    if (!entities.isArray() || id.empty())
        return nullptr;
    for (Json::Value& entity : entities)
    {
        if (entity.isObject() && entity["id"].isString() && entity["id"].asString() == id)
            return &entity;
    }
    return nullptr;
}

ecs::Entity FindEntityByAuthoringId(SceneEntityWorld& world, const std::string& id)
{
    if (id.empty())
        return ecs::NullEntity;

    ecs::Entity found = ecs::NullEntity;
    world.world().each<SceneAuthoringIdComponent>(
        [&](ecs::Entity entity, const SceneAuthoringIdComponent& authoring)
        {
            if (!ecs::isValid(found) && authoring.id == id)
                found = entity;
        });
    if (ecs::isValid(found))
        return found;

    world.world().each<NameComponent>(
        [&](ecs::Entity entity, const NameComponent& name)
        {
            if (!ecs::isValid(found) && name.value == id)
                found = entity;
        });
    return found;
}

void WriteDouble3(Json::Value& node, const math::double3& value)
{
    Json::Value array(Json::arrayValue);
    caustica::json::write(array, value);
    node = std::move(array);
}

void WriteTransformComponent(Json::Value& transform, const LocalTransformComponent& local)
{
    if (!transform.isObject())
        transform = Json::Value(Json::objectValue);

    WriteDouble3(transform["translation"], local.translation);
    {
        Json::Value rotation(Json::arrayValue);
        const math::double4 xyzw(
            local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w);
        caustica::json::write(rotation, xyzw);
        transform["rotation"] = std::move(rotation);
    }
    WriteDouble3(transform["scale"], local.scaling);
    transform.removeMember("euler");
}

Json::Value& EnsureObject(Json::Value& node)
{
    if (!node.isObject())
        node = Json::Value(Json::objectValue);
    return node;
}

Json::Value& EnsureComponent(Json::Value& entityNode, const char* name)
{
    return EnsureObject(EnsureObject(entityNode["components"])[name]);
}

bool HasAuthoringId(const SceneEntityWorld& world, ecs::Entity entity)
{
    return world.world().tryGet<SceneAuthoringIdComponent>(entity) != nullptr;
}

void WriteDirectionalLight(Json::Value& node, const DirectionalLightComponent& light)
{
    node["enabled"] << light.enabled;
    node["color"] << light.color;
    node["irradiance"] << light.irradiance;
    node["angularSize"] << light.angularSize;
}

void WritePointLight(Json::Value& node, const PointLightComponent& light)
{
    node["enabled"] << light.enabled;
    node["color"] << light.color;
    node["intensity"] << light.intensity;
    node["radius"] << light.radius;
    node["range"] << light.range;
}

void WriteSpotLight(Json::Value& node, const SpotLightComponent& light)
{
    node["enabled"] << light.enabled;
    node["color"] << light.color;
    node["intensity"] << light.intensity;
    node["radius"] << light.radius;
    node["range"] << light.range;
    node["innerAngle"] << light.innerAngle;
    node["outerAngle"] << light.outerAngle;
}

void WriteRectLight(Json::Value& node, const RectLightComponent& light)
{
    node["enabled"] << light.enabled;
    node["color"] << light.color;
    node["intensity"] << light.intensity;
    node["width"] << light.width;
    node["height"] << light.height;
}

void WriteEnvironmentLight(Json::Value& node, const EnvironmentLightComponent& light, bool full)
{
    node["enabled"] << light.enabled;
    if (!full)
        return;
    node["radianceScale"] << light.radianceScale;
    node["rotation"] << light.rotation;
    if (!light.path.empty())
        node["source"] << light.path;
}

void WriteCameraIntoComponents(Json::Value& components, const CameraComponent& camera)
{
    EnsureObject(components);
    if (const PerspectiveCameraData* pers = tryGetPerspectiveCameraData(camera))
    {
        const char* key = "PerspectiveCameraEx";
        if (components.isMember("PerspectiveCamera") && !components.isMember("PerspectiveCameraEx"))
            key = "PerspectiveCamera";
        Json::Value& cam = EnsureObject(components[key]);
        cam["verticalFov"] << pers->verticalFov;
        cam["zNear"] << pers->zNear;
        // Patch an existing authoring node in place. Remove optional values
        // first so clearing a runtime property does not resurrect it after a
        // save/reload cycle.
        cam.removeMember("zFar");
        cam.removeMember("aspectRatio");
        cam.removeMember("fx");
        cam.removeMember("fy");
        cam.removeMember("cx");
        cam.removeMember("cy");
        cam.removeMember("width");
        cam.removeMember("height");
        if (pers->zFar)
            cam["zFar"] << *pers->zFar;
        if (pers->aspectRatio)
            cam["aspectRatio"] << *pers->aspectRatio;
        if (pers->intrinsics)
        {
            cam["fx"] << pers->intrinsics->fx;
            cam["fy"] << pers->intrinsics->fy;
            cam["cx"] << pers->intrinsics->cx;
            cam["cy"] << pers->intrinsics->cy;
            cam["width"] << pers->intrinsics->width;
            cam["height"] << pers->intrinsics->height;
        }
        return;
    }

    if (const OrthographicCameraData* ortho = tryGetOrthographicCameraData(camera))
    {
        Json::Value& cam = EnsureObject(components["OrthographicCamera"]);
        cam["xMag"] << ortho->xMag;
        cam["yMag"] << ortho->yMag;
        cam["zNear"] << ortho->zNear;
        cam["zFar"] << ortho->zFar;
    }
}

void WriteInspectorComponents(Json::Value& entityNode, SceneEntityWorld& world, ecs::Entity entity)
{
    if (const DirectionalLightComponent* light = tryGetDirectionalLight(world.world(), entity))
        WriteDirectionalLight(EnsureComponent(entityNode, "DirectionalLight"), *light);
    if (const PointLightComponent* light = tryGetPointLight(world.world(), entity))
        WritePointLight(EnsureComponent(entityNode, "PointLight"), *light);
    if (const SpotLightComponent* light = tryGetSpotLight(world.world(), entity))
        WriteSpotLight(EnsureComponent(entityNode, "SpotLight"), *light);
    if (const RectLightComponent* light = tryGetRectLight(world.world(), entity))
        WriteRectLight(EnsureComponent(entityNode, "RectLight"), *light);
    if (const EnvironmentLightComponent* light = tryGetEnvironmentLight(world.world(), entity))
        WriteEnvironmentLight(EnsureComponent(entityNode, "EnvironmentLight"), *light, false);
    if (const CameraComponent* camera = tryGetCamera(world.world(), entity))
        WriteCameraIntoComponents(EnsureObject(entityNode["components"]), *camera);
    if (const GaussianSplatComponent* splat = world.world().tryGet<GaussianSplatComponent>(entity))
        EnsureComponent(entityNode, "GaussianSplat")["enabled"] << splat->splat.enabled;
    if (const auto* label = world.world().tryGet<SemanticLabelComponent>(entity))
    {
        Json::Value& node = EnsureComponent(entityNode, "SemanticLabel");
        if (label->instanceId != 0)
            node["instance_id"] = label->instanceId;
        if (label->semanticId != 0)
            node["semantic_id"] = label->semanticId;
        if (!label->semanticLabel.empty())
            node["class"] = label->semanticLabel;
    }
}

bool HasInspectorComponents(SceneEntityWorld& world, ecs::Entity entity)
{
    return tryGetDirectionalLight(world.world(), entity)
        || tryGetPointLight(world.world(), entity)
        || tryGetSpotLight(world.world(), entity)
        || tryGetRectLight(world.world(), entity)
        || tryGetEnvironmentLight(world.world(), entity)
        || tryGetCamera(world.world(), entity)
        || world.world().tryGet<GaussianSplatComponent>(entity)
        || world.world().tryGet<SemanticLabelComponent>(entity);
}

std::string EntityPathString(SceneEntityWorld& world, ecs::Entity entity)
{
    return world.getEntityPath(entity).generic_string();
}

template<typename WriteFn>
void AppendOverrideIfUnauthored(
    Json::Value& overrides,
    SceneEntityWorld& world,
    ecs::Entity entity,
    const char* componentName,
    WriteFn&& write)
{
    if (HasAuthoringId(world, entity))
        return;
    const std::string path = EntityPathString(world, entity);
    if (path.empty())
        return;
    Json::Value patch(Json::objectValue);
    patch["path"] = path;
    write(EnsureObject(patch[componentName]));
    overrides.append(std::move(patch));
}

void DisableEntityVisibility(SceneEntityWorld& world, ecs::Entity entity)
{
    if (MeshInstanceComponent* mesh = world.world().tryGet<MeshInstanceComponent>(entity))
    {
        mesh->enabled = false;
        world.world().notifyComponentChanged<MeshInstanceComponent>(entity);
    }
    if (GaussianSplatComponent* splat = world.world().tryGet<GaussianSplatComponent>(entity))
    {
        splat->splat.enabled = false;
        world.world().notifyComponentChanged<GaussianSplatComponent>(entity);
    }
    if (DirectionalLightComponent* light = tryGetDirectionalLight(world.world(), entity))
    {
        light->enabled = false;
        world.world().notifyComponentChanged<DirectionalLightComponent>(entity);
    }
    if (PointLightComponent* light = tryGetPointLight(world.world(), entity))
    {
        light->enabled = false;
        world.world().notifyComponentChanged<PointLightComponent>(entity);
    }
    if (SpotLightComponent* light = tryGetSpotLight(world.world(), entity))
    {
        light->enabled = false;
        world.world().notifyComponentChanged<SpotLightComponent>(entity);
    }
    if (RectLightComponent* light = tryGetRectLight(world.world(), entity))
    {
        light->enabled = false;
        world.world().notifyComponentChanged<RectLightComponent>(entity);
    }
    if (EnvironmentLightComponent* light = tryGetEnvironmentLight(world.world(), entity))
    {
        light->enabled = false;
        world.world().notifyComponentChanged<EnvironmentLightComponent>(entity);
    }
}

} // namespace

Json::Value mergeSceneOverlay(const Json::Value& base, const Json::Value& overlay)
{
    Json::Value merged = base;
    if (!merged.isObject())
        merged = Json::Value(Json::objectValue);

    if (overlay.isMember("settings") && overlay["settings"].isObject())
        MergeObjectFields(merged["settings"], overlay["settings"]);

    if (overlay.isMember("name") && overlay["name"].isString())
        merged["name"] = overlay["name"];

    if (!merged.isMember("entities") || !merged["entities"].isArray())
        merged["entities"] = Json::Value(Json::arrayValue);

    const Json::Value& overrides = overlay["overrides"];
    if (overrides.isArray())
    {
        for (const Json::Value& patch : overrides)
        {
            if (!patch.isObject() || !patch["id"].isString())
                continue;

            const std::string id = patch["id"].asString();
            Json::Value* target = FindEntityById(merged["entities"], id);
            if (!target)
            {
                merged["entities"].append(patch);
                continue;
            }

            if (patch.isMember("name"))
                (*target)["name"] = patch["name"];
            if (patch.isMember("parent"))
                (*target)["parent"] = patch["parent"];
            if (patch.isMember("components") && patch["components"].isObject())
            {
                if (!(*target)["components"].isObject())
                    (*target)["components"] = Json::Value(Json::objectValue);
                const auto componentNames = patch["components"].getMemberNames();
                for (const auto& componentName : componentNames)
                    MergeObjectFields((*target)["components"][componentName], patch["components"][componentName]);
            }
        }
    }

    if (overlay.isMember("animations"))
        merged["animations"] = overlay["animations"];

    return merged;
}

std::string canonicalizeEnvSource(std::string source)
{
    auto trim = [](std::string value)
    {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch);
        }).base();
        if (begin >= end)
            return std::string();
        return std::string(begin, end);
    };

    source = trim(std::move(source));
    std::string lower = source;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "procedural:sky" || lower == "procedural:sky:default")
        return c_EnvMapProcSky;
    if (lower == "procedural:sky:morning")
        return c_EnvMapProcSky_Morning;
    if (lower == "procedural:sky:midday")
        return c_EnvMapProcSky_Midday;
    if (lower == "procedural:sky:evening")
        return c_EnvMapProcSky_Evening;
    if (lower == "procedural:sky:dawn")
        return c_EnvMapProcSky_Dawn;
    if (lower == "procedural:sky:pitchblack" || lower == "procedural:sky:pitch-black")
        return c_EnvMapProcSky_PitchBlack;

    return source;
}

void applyAuthoringTransform(
    SceneEntityWorld& world,
    ecs::Entity entity,
    const Json::Value& transform)
{
    if (!ecs::isValid(entity) || !transform.isObject())
        return;

    if (!transform["translation"].isNull())
    {
        math::double3 value = math::double3::zero();
        transform["translation"] >> value;
        world.setTranslation(entity, value);
    }

    const Json::Value& rotation = transform["rotation"];
    if (rotation.isArray() && rotation.size() >= 3)
    {
        // Legacy scenes: a 3-element rotation is xyz with w defaulting to 1.
        math::double4 value = math::double4(0.0, 0.0, 0.0, 1.0);
        if (rotation.size() >= 4)
            rotation >> value;
        else
        {
            value.x = rotation[0].asDouble();
            value.y = rotation[1].asDouble();
            value.z = rotation[2].asDouble();
        }
        world.setRotation(entity, math::dquat::fromXYZW(value));
    }
    else if (!transform["euler"].isNull())
    {
        math::double3 value = math::double3::zero();
        transform["euler"] >> value;
        world.setRotation(entity, rotationQuat(value));
    }

    if (!transform["scale"].isNull())
    {
        math::double3 value = math::double3(1.0);
        transform["scale"] >> value;
        world.setScaling(entity, value);
    }
}

void patchEntityTransforms(
    Json::Value& entities,
    SceneEntityWorld& world)
{
    if (!entities.isArray())
        return;

    for (Json::Value& entityNode : entities)
    {
        if (!entityNode.isObject())
            continue;

        std::string id;
        if (entityNode["id"].isString())
            id = entityNode["id"].asString();
        else if (entityNode["name"].isString())
            id = entityNode["name"].asString();

        const ecs::Entity entity = FindEntityByAuthoringId(world, id);
        if (!ecs::isValid(entity))
            continue;

        const auto* local = world.world().tryGet<LocalTransformComponent>(entity);
        const auto* prefab = world.world().tryGet<PrefabInstanceComponent>(entity);
        const bool hasLook = HasInspectorComponents(world, entity);
        if ((!local || !local->hasLocalTransform) && prefab == nullptr && !hasLook)
            continue;

        if (!entityNode["components"].isObject())
            entityNode["components"] = Json::Value(Json::objectValue);

        if (local && local->hasLocalTransform)
            WriteTransformComponent(entityNode["components"]["Transform"], *local);

        if (prefab)
        {
            Json::Value& prefabNode = entityNode["components"]["PrefabInstance"];
            if (!prefabNode.isObject())
                prefabNode = Json::Value(Json::objectValue);
            prefabNode["source"] = prefab->source;
            if (prefab->materials.empty())
                prefabNode.removeMember("materials");
            else
            {
                Json::Value materials(Json::objectValue);
                for (const auto& [slot, path] : prefab->materials)
                    materials[slot] = path;
                prefabNode["materials"] = std::move(materials);
            }
        }

        WriteInspectorComponents(entityNode, world, entity);
    }
}

void patchEntityOverrides(
    Json::Value& document,
    SceneEntityWorld& world)
{
    Json::Value overrides(Json::arrayValue);

    world.world().each<DirectionalLightComponent>(
        [&](ecs::Entity entity, const DirectionalLightComponent& light)
        {
            AppendOverrideIfUnauthored(overrides, world, entity, "DirectionalLight",
                [&](Json::Value& node) { WriteDirectionalLight(node, light); });
        });
    world.world().each<PointLightComponent>(
        [&](ecs::Entity entity, const PointLightComponent& light)
        {
            AppendOverrideIfUnauthored(overrides, world, entity, "PointLight",
                [&](Json::Value& node) { WritePointLight(node, light); });
        });
    world.world().each<SpotLightComponent>(
        [&](ecs::Entity entity, const SpotLightComponent& light)
        {
            AppendOverrideIfUnauthored(overrides, world, entity, "SpotLight",
                [&](Json::Value& node) { WriteSpotLight(node, light); });
        });
    world.world().each<RectLightComponent>(
        [&](ecs::Entity entity, const RectLightComponent& light)
        {
            AppendOverrideIfUnauthored(overrides, world, entity, "RectLight",
                [&](Json::Value& node) { WriteRectLight(node, light); });
        });
    world.world().each<EnvironmentLightComponent>(
        [&](ecs::Entity entity, const EnvironmentLightComponent& light)
        {
            AppendOverrideIfUnauthored(overrides, world, entity, "EnvironmentLight",
                [&](Json::Value& node) { WriteEnvironmentLight(node, light, true); });
        });
    world.world().each<CameraComponent>(
        [&](ecs::Entity entity, const CameraComponent& camera)
        {
            if (HasAuthoringId(world, entity))
                return;
            const std::string path = EntityPathString(world, entity);
            if (path.empty())
                return;
            Json::Value patch(Json::objectValue);
            patch["path"] = path;
            WriteCameraIntoComponents(EnsureObject(patch), camera);
            overrides.append(std::move(patch));
        });

    if (overrides.empty())
        document.removeMember("entityOverrides");
    else
        document["entityOverrides"] = std::move(overrides);
}

void upsertAuthoredEntityNode(
    Json::Value& document,
    SceneEntityWorld& world,
    ecs::Entity entity)
{
    if (!ecs::isValid(entity) || entity == world.root())
        return;

    const auto* authoring = world.world().tryGet<SceneAuthoringIdComponent>(entity);
    if (!authoring || authoring->id.empty())
        return;

    if (!document.isObject())
        document = Json::Value(Json::objectValue);
    if (!document["entities"].isArray())
        document["entities"] = Json::Value(Json::arrayValue);

    Json::Value* existing = FindEntityById(document["entities"], authoring->id);
    Json::Value created(Json::objectValue);
    Json::Value& entityNode = existing ? *existing : created;

    entityNode["id"] = authoring->id;
    const std::string name = world.getEntityName(entity);
    entityNode["name"] = name.empty() ? authoring->id : name;

    bool wroteParent = false;
    if (const auto* parent = world.world().tryGet<ParentComponent>(entity))
    {
        if (ecs::isValid(parent->parent) && parent->parent != world.root())
        {
            if (const auto* parentAuth = world.world().tryGet<SceneAuthoringIdComponent>(parent->parent))
            {
                entityNode["parent"] = parentAuth->id;
                wroteParent = true;
            }
        }
    }
    if (!wroteParent)
        entityNode.removeMember("parent");

    if (!entityNode["components"].isObject())
        entityNode["components"] = Json::Value(Json::objectValue);

    if (const auto* local = world.world().tryGet<LocalTransformComponent>(entity))
    {
        if (local->hasLocalTransform)
            WriteTransformComponent(entityNode["components"]["Transform"], *local);
    }

    if (const auto* prefab = world.world().tryGet<PrefabInstanceComponent>(entity))
    {
        Json::Value& prefabNode = EnsureObject(entityNode["components"]["PrefabInstance"]);
        prefabNode["source"] = prefab->source;
        if (prefab->materials.empty())
            prefabNode.removeMember("materials");
        else
        {
            Json::Value materials(Json::objectValue);
            for (const auto& [slot, path] : prefab->materials)
                materials[slot] = path;
            prefabNode["materials"] = std::move(materials);
        }
    }

    if (const EnvironmentLightComponent* light = tryGetEnvironmentLight(world.world(), entity))
        WriteEnvironmentLight(EnsureComponent(entityNode, "EnvironmentLight"), *light, true);
    WriteInspectorComponents(entityNode, world, entity);

    if (!existing)
        document["entities"].append(std::move(created));
}

void removeAuthoredEntityNode(Json::Value& document, const std::string& id)
{
    if (id.empty() || !document["entities"].isArray())
        return;

    Json::Value kept(Json::arrayValue);
    for (const Json::Value& entity : document["entities"])
    {
        if (!entity.isObject())
            continue;
        std::string entityId;
        if (entity["id"].isString())
            entityId = entity["id"].asString();
        else if (entity["name"].isString())
            entityId = entity["name"].asString();
        if (entityId == id)
            continue;
        kept.append(entity);
    }
    document["entities"] = std::move(kept);
}

void syncAuthoredEntitiesToDocument(Json::Value& document, SceneEntityWorld& world)
{
    world.world().each<SceneAuthoringIdComponent>(
        [&](ecs::Entity entity, const SceneAuthoringIdComponent&)
        {
            upsertAuthoredEntityNode(document, world, entity);
        });
}

void applyEntityOverrides(
    SceneEntityWorld& world,
    const Json::Value& overrides)
{
    if (!overrides.isArray())
        return;

    for (const Json::Value& patch : overrides)
    {
        if (!patch.isObject() || !patch["path"].isString())
            continue;

        const ecs::Entity entity = world.entityForPath(patch["path"].asString());
        if (!ecs::isValid(entity))
        {
            caustica::warning("entityOverrides path '%s' not found, skipping.",
                patch["path"].asCString());
            continue;
        }

        auto applyLight = [&](const char* type)
        {
            if (!patch.isMember(type) || !patch[type].isObject())
                return;
            auto component = makeLightComponentFromJson(type, patch[type]);
            if (!component)
                return;
            std::visit(
                [&](auto&& light)
                {
                    using T = std::decay_t<decltype(light)>;
                    if constexpr (std::is_same_v<T, DirectionalLightComponent>)
                    {
                        if (tryGetDirectionalLight(world.world(), entity))
                            world.setDirectionalLight(entity, std::move(light));
                    }
                    else if constexpr (std::is_same_v<T, PointLightComponent>)
                    {
                        if (tryGetPointLight(world.world(), entity))
                            world.setPointLight(entity, std::move(light));
                    }
                    else if constexpr (std::is_same_v<T, SpotLightComponent>)
                    {
                        if (tryGetSpotLight(world.world(), entity))
                            world.setSpotLight(entity, std::move(light));
                    }
                    else if constexpr (std::is_same_v<T, RectLightComponent>)
                    {
                        if (tryGetRectLight(world.world(), entity))
                            world.setRectLight(entity, std::move(light));
                    }
                    else if constexpr (std::is_same_v<T, EnvironmentLightComponent>)
                    {
                        if (tryGetEnvironmentLight(world.world(), entity))
                            world.setEnvironmentLight(entity, std::move(light));
                    }
                },
                std::move(*component));
        };

        applyLight("DirectionalLight");
        applyLight("PointLight");
        applyLight("SpotLight");
        applyLight("RectLight");
        applyLight("EnvironmentLight");

        const char* cameraType = nullptr;
        if (patch.isMember("PerspectiveCameraEx") && patch["PerspectiveCameraEx"].isObject())
            cameraType = "PerspectiveCameraEx";
        else if (patch.isMember("PerspectiveCamera") && patch["PerspectiveCamera"].isObject())
            cameraType = "PerspectiveCamera";
        else if (patch.isMember("OrthographicCamera") && patch["OrthographicCamera"].isObject())
            cameraType = "OrthographicCamera";
        if (cameraType && tryGetCamera(world.world(), entity))
        {
            if (auto component = makeCameraComponentFromJson(cameraType, patch[cameraType]))
                world.setCamera(entity, std::move(*component));
        }
    }
}

void applyHiddenEntities(
    SceneEntityWorld& world,
    const std::vector<std::string>& paths)
{
    for (const std::string& path : paths)
    {
        if (path.empty())
            continue;
        const ecs::Entity entity = world.entityForPath(path);
        if (!ecs::isValid(entity))
        {
            caustica::warning("hiddenEntities path '%s' not found, skipping.", path.c_str());
            continue;
        }
        DisableEntityVisibility(world, entity);
    }
}

} // namespace caustica::scene

namespace caustica
{
namespace
{

std::unordered_map<std::string, std::string> ReadNamedMaterialMap(const Json::Value& node)
{
    std::unordered_map<std::string, std::string> slots;
    if (!node.isObject())
        return slots;
    const auto names = node.getMemberNames();
    for (const auto& name : names)
    {
        if (node[name].isString())
            slots[name] = node[name].asString();
    }
    return slots;
}

std::unordered_map<std::string, std::string> ReadPrefabMaterialSlots(const Json::Value& prefab)
{
    return ReadNamedMaterialMap(prefab["materials"]);
}

std::unordered_map<std::string, std::string> ReadMaterialOverrideSlots(const Json::Value& node)
{
    std::unordered_map<std::string, std::string> slots;
    if (node.isString())
    {
        slots.emplace("*", node.asString());
        return slots;
    }
    if (!node.isObject())
        return slots;
    if (node["source"].isString())
        slots.emplace("*", node["source"].asString());
    auto named = ReadNamedMaterialMap(node["slots"]);
    slots.insert(named.begin(), named.end());
    return slots;
}

std::string ReadEntityId(const Json::Value& src, const std::string& fallbackName)
{
    if (src["id"].isString() && !src["id"].asString().empty())
        return src["id"].asString();
    return fallbackName;
}

std::string ReadEntityName(const Json::Value& src)
{
    if (src["name"].isString())
        return src["name"].asString();
    if (src["id"].isString())
        return src["id"].asString();
    return {};
}

const Json::Value& ComponentNode(const Json::Value& components, const char* name)
{
    static const Json::Value kNull;
    if (!components.isObject() || !components.isMember(name))
        return kNull;
    return components[name];
}

} // namespace

void Scene::applyTopLevelSettings(const Json::Value& settingsNode)
{
    if (!settingsNode.isObject() || !m_EntityWorld)
        return;

    SceneSettings settings;
    settings.name = "SceneSettings";
    settings.load(settingsNode);

    ecs::Entity settingsEntity = ecs::NullEntity;
    m_EntityWorld->world().each<scene::SceneSettingsComponent>(
        [&](ecs::Entity entity, scene::SceneSettingsComponent&)
        {
            if (!ecs::isValid(settingsEntity))
                settingsEntity = entity;
        });
    if (!ecs::isValid(settingsEntity))
        settingsEntity = m_EntityWorld->createEntity("SceneSettings", m_EntityWorld->root());
    m_EntityWorld->setSceneSettings(settingsEntity, settings);
}

bool Scene::instantiateEntities(
    scene::SceneEntityWorld& world,
    ecs::Entity defaultParent,
    const Json::Value& documentRoot,
    bool asyncTextures)
{
    const Json::Value& entities = documentRoot["entities"];
    if (!entities.isArray())
    {
        caustica::error("Scene JSON is missing entities[].");
        return false;
    }

    std::unordered_map<std::string, ecs::Entity> idToEntity;
    std::unordered_map<std::string, const Json::Value*> idToDef;
    std::vector<std::string> order;
    order.reserve(entities.size());

    for (const Json::Value& src : entities)
    {
        if (!src.isObject())
            continue;
        const std::string name = ReadEntityName(src);
        const std::string id = ReadEntityId(src, name);
        if (id.empty())
        {
            caustica::warning("Skipping unnamed scene entity.");
            continue;
        }
        if (idToDef.contains(id))
        {
            caustica::warning("Duplicate scene entity id '%s', skipping.", id.c_str());
            continue;
        }
        idToDef.emplace(id, &src);
        order.push_back(id);
    }

    auto instantiateOne = [&](auto& self, const std::string& id) -> ecs::Entity
    {
        if (auto it = idToEntity.find(id); it != idToEntity.end())
            return it->second;

        const auto defIt = idToDef.find(id);
        if (defIt == idToDef.end())
            return ecs::NullEntity;

        const Json::Value& src = *defIt->second;
        idToEntity.emplace(id, ecs::NullEntity);

        ecs::Entity parent = defaultParent;
        if (src["parent"].isString())
        {
            const std::string parentId = src["parent"].asString();
            parent = self(self, parentId);
            if (!ecs::isValid(parent))
            {
                caustica::warning("Scene entity '%s' parent '%s' not found, parenting to default parent.",
                    id.c_str(), parentId.c_str());
                parent = defaultParent;
            }
        }

        const std::string name = ReadEntityName(src);
        const Json::Value& components = src["components"];
        const Json::Value& prefab = ComponentNode(components, "PrefabInstance");
        const Json::Value& materialOverride = ComponentNode(components, "MaterialOverride");

        std::unordered_map<std::string, std::string> materialSlots = ReadPrefabMaterialSlots(prefab);
        auto overrideSlots = ReadMaterialOverrideSlots(materialOverride);
        materialSlots.insert(overrideSlots.begin(), overrideSlots.end());

        ecs::Entity entity = ecs::NullEntity;
        if (prefab.isObject())
        {
            std::string source;
            if (prefab["source"].isString())
                source = prefab["source"].asString();
            entity = instantiatePrefab(source, parent, name, asyncTextures, &world, materialSlots);
        }
        else
        {
            entity = world.createEntity(name, parent);
            if (!materialSlots.empty())
                applyMaterialSlots(world, entity, materialSlots);
        }

        if (!ecs::isValid(entity))
        {
            idToEntity[id] = ecs::NullEntity;
            return ecs::NullEntity;
        }

        world.world().emplace<scene::SceneAuthoringIdComponent>(
            entity, scene::SceneAuthoringIdComponent{ id });
        if (!overrideSlots.empty() || materialOverride.isObject() || materialOverride.isString())
        {
            scene::MaterialOverrideComponent component;
            if (materialOverride.isString())
                component.source = materialOverride.asString();
            else if (materialOverride["source"].isString())
                component.source = materialOverride["source"].asString();
            component.slots = std::move(overrideSlots);
            world.world().emplace<scene::MaterialOverrideComponent>(entity, std::move(component));
        }

        if (!name.empty())
        {
            if (auto* nameComp = world.world().get<scene::NameComponent>(entity))
                nameComp->value = name;
        }

        scene::applyAuthoringTransform(world, entity, ComponentNode(components, "Transform"));

        if (components.isObject())
        {
            const auto names = components.getMemberNames();
            for (const auto& componentName : names)
            {
                if (componentName == "Transform"
                    || componentName == "PrefabInstance"
                    || componentName == "MaterialOverride")
                    continue;

                Json::Value leaf = components[componentName];
                if (!leaf.isObject())
                    leaf = Json::Value(Json::objectValue);
                leaf["type"] = componentName;
                if (componentName == "EnvironmentLight")
                {
                    if (leaf["source"].isString())
                        leaf["path"] = scene::canonicalizeEnvSource(leaf["source"].asString());
                }
                attachLeafFromJson(world, entity, leaf);
            }
        }

        idToEntity[id] = entity;
        return entity;
    };

    for (const std::string& id : order)
        instantiateOne(instantiateOne, id);

    return true;
}

bool Scene::loadEntities(
    Json::Value documentRoot,
    const std::filesystem::path& scenePath,
    bool asyncTextures)
{
    if (documentRoot["base"].isString())
    {
        const std::string baseRef = documentRoot["base"].asString();
        const std::filesystem::path basePath = resolveSceneMediaPath(baseRef, scenePath);
        Json::Value baseDocument;
        if (!caustica::json::loadFromFile(*m_fs, basePath, baseDocument))
        {
            caustica::error("Failed to load scene base '%s' (from '%s').",
                basePath.generic_string().c_str(), baseRef.c_str());
            return false;
        }
        if (!baseDocument.isObject() || !baseDocument["entities"].isArray())
        {
            caustica::error("Scene overlay base '%s' has no entities[].",
                basePath.generic_string().c_str());
            return false;
        }
        documentRoot = scene::mergeSceneOverlay(baseDocument, documentRoot);
    }

    if (!loadCustomData(documentRoot, asyncTextures))
        return false;

    if (!instantiateEntities(*m_EntityWorld, m_EntityWorld->root(), documentRoot, asyncTextures))
        return false;

    if (documentRoot.isMember("settings"))
        applyTopLevelSettings(documentRoot["settings"]);

    loadAnimations(documentRoot["animations"]);
    m_EntityWorld->rebuildPathsFromRoot();
    scene::applyEntityOverrides(*m_EntityWorld, documentRoot["entityOverrides"]);
    if (documentRoot.isMember("settings") && documentRoot["settings"].isObject())
    {
        SceneSettings look;
        look.load(documentRoot["settings"]);
        scene::applyHiddenEntities(*m_EntityWorld, look.hiddenEntities);
    }
    return true;
}

} // namespace caustica
