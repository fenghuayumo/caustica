#include <scene/SceneInspection.h>
#include <scene/SceneEcs.h>
#include <scene/ScenePoseAccess.h>
#include <scene/SceneSerializer.h>
#include <core/json.h>

#include <algorithm>
#include <cmath>

namespace caustica::scene
{
namespace
{
std::string runtimeId(ecs::Entity entity)
{
    return std::to_string(entt::to_integral(entity));
}

Json::Value poseValue(const EntityPose& pose)
{
    if (!math::all(math::isfinite(pose.position)) || !math::all(math::isfinite(pose.scaling))
        || !std::isfinite(pose.rotation.x) || !std::isfinite(pose.rotation.y)
        || !std::isfinite(pose.rotation.z) || !std::isfinite(pose.rotation.w))
        return Json::Value();
    Json::Value result(Json::objectValue);
    result["translation"] << pose.position;
    result["rotation"] << math::double4(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w);
    result["scale"] << pose.scaling;
    return result;
}
}

Json::Value inspectSceneEntities(const SceneEntityWorld& world, size_t offset, size_t limit)
{
    std::vector<ecs::Entity> entities;
    world.world().each<NameComponent>([&](ecs::Entity entity, const NameComponent&) {
        if (entity != world.root())
            entities.push_back(entity);
    });
    std::sort(entities.begin(), entities.end(), [](ecs::Entity a, ecs::Entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    Json::Value result(Json::objectValue);
    result["entities"] = Json::Value(Json::arrayValue);
    result["total"] = Json::UInt64(entities.size());
    result["offset"] = Json::UInt64(offset);
    limit = std::min(limit, size_t(1000));
    result["limit"] = Json::UInt64(limit);
    const size_t begin = std::min(offset, entities.size());
    const size_t end = begin + std::min(limit, entities.size() - begin);
    for (size_t index = begin; index < end; ++index)
    {
        const auto entity = entities[index];
        Json::Value item(Json::objectValue);
        item["runtime_id"] = runtimeId(entity);
        item["authored_id"] = Json::Value();
        if (const auto* authoring = world.world().tryGet<SceneAuthoringIdComponent>(entity))
            item["authored_id"] = authoring->id;
        // IDs inside a .prefab.json belong to that instance, not the enclosing
        // scene document. Two instances may contain the same internal ID.
        item["authored_scope_runtime_id"] = Json::Value();
        auto ancestor = entity;
        while (const auto* parent = world.world().tryGet<ParentComponent>(ancestor))
        {
            ancestor = parent->parent;
            if (!ecs::isValid(ancestor) || ancestor == world.root())
                break;
            if (world.world().tryGet<PrefabInstanceComponent>(ancestor))
            {
                item["authored_scope_runtime_id"] = runtimeId(ancestor);
                break;
            }
        }
        item["name"] = world.getEntityName(entity);
        item["path"] = world.getEntityPath(entity).generic_string();
        item["parent_runtime_id"] = Json::Value();
        if (const auto* parent = world.world().tryGet<ParentComponent>(entity))
            if (ecs::isValid(parent->parent) && parent->parent != world.root())
                item["parent_runtime_id"] = runtimeId(parent->parent);
        item["children_runtime_ids"] = Json::Value(Json::arrayValue);
        for (const auto child : world.getEntityChildren(entity))
            item["children_runtime_ids"].append(runtimeId(child));
        EntityPose pose;
        if (getEntityLocalPose(world, entity, pose))
            item["local_pose"] = poseValue(pose);
        if (getEntityWorldPose(world, entity, pose))
            item["world_pose"] = poseValue(pose);
        item["bounds"] = Json::Value();
        if (const auto* bounds = world.world().tryGet<BoundsComponent>(entity))
        {
            const auto& box = bounds->globalBounds;
            if (!box.isempty() && math::all(math::isfinite(box.m_mins)) && math::all(math::isfinite(box.m_maxs)))
            {
                item["bounds"]["min"] << box.m_mins;
                item["bounds"]["max"] << box.m_maxs;
            }
        }
        item["kinds"] = Json::Value(Json::arrayValue);
        item["material_slots"] = Json::Value(Json::arrayValue);
        if (const auto* mesh = world.world().tryGet<MeshInstanceComponent>(entity))
        {
            item["kinds"].append("mesh");
            item["enabled"] = mesh->enabled;
            if (mesh->mesh)
                for (size_t slot = 0; slot < mesh->mesh->geometries.size(); ++slot)
                {
                    const auto& geometry = mesh->mesh->geometries[slot];
                    if (!geometry || !geometry->material)
                        continue;
                    Json::Value material(Json::objectValue);
                    material["slot"] = Json::UInt64(slot);
                    material["name"] = geometry->material->name;
                    material["source"] = geometry->material->overrideSource;
                    item["material_slots"].append(std::move(material));
                }
        }
        if (world.world().tryGet<CameraComponent>(entity))
            item["kinds"].append("camera");
        if (world.world().tryGet<DirectionalLightComponent>(entity)
            || world.world().tryGet<PointLightComponent>(entity)
            || world.world().tryGet<SpotLightComponent>(entity)
            || world.world().tryGet<RectLightComponent>(entity)
            || world.world().tryGet<EnvironmentLightComponent>(entity))
            item["kinds"].append("light");
        if (const auto* prefab = world.world().tryGet<PrefabInstanceComponent>(entity))
        {
            item["kinds"].append("prefab");
            item["asset_source"] = prefab->source;
        }
        if (world.world().tryGet<GaussianSplatComponent>(entity))
            item["kinds"].append("gaussian_splat");
        if (item["kinds"].empty())
            item["kinds"].append("group");
        // Reuse the save serializer for authored light/camera parameters.
        if (!item["authored_id"].isNull())
        {
            Json::Value document(Json::objectValue);
            upsertAuthoredEntityNode(document, world, entity);
            if (document["entities"].size() == 1)
                item["components"] = document["entities"][0]["components"];
        }
        result["entities"].append(std::move(item));
    }
    return result;
}
}
