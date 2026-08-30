#include <scene/SceneSerializer.h>

#include <scene/Scene.h>
#include <scene/SceneComponentBuilders.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <core/json.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <math/math.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <utility>

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

void WriteDouble3(Json::Value& node, const dm::double3& value)
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
        const dm::double4 xyzw(
            local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w);
        caustica::json::write(rotation, xyzw);
        transform["rotation"] = std::move(rotation);
    }
    WriteDouble3(transform["scale"], local.scaling);
    transform.removeMember("euler");
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
        dm::double3 value = dm::double3::zero();
        transform["translation"] >> value;
        world.setTranslation(entity, value);
    }

    const Json::Value& rotation = transform["rotation"];
    if (rotation.isArray() && rotation.size() >= 3)
    {
        // Donut accepted a 3-element rotation as xyz with w defaulting to 1.
        dm::double4 value = dm::double4(0.0, 0.0, 0.0, 1.0);
        if (rotation.size() >= 4)
            rotation >> value;
        else
        {
            value.x = rotation[0].asDouble();
            value.y = rotation[1].asDouble();
            value.z = rotation[2].asDouble();
        }
        world.setRotation(entity, dm::dquat::fromXYZW(value));
    }
    else if (!transform["euler"].isNull())
    {
        dm::double3 value = dm::double3::zero();
        transform["euler"] >> value;
        world.setRotation(entity, rotationQuat(value));
    }

    if (!transform["scale"].isNull())
    {
        dm::double3 value = dm::double3(1.0);
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
        if ((!local || !local->hasLocalTransform) && prefab == nullptr)
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
    return true;
}

} // namespace caustica
