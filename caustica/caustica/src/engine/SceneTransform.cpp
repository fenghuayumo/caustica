#include <engine/SceneTransform.h>

#include <engine/App.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/ScenePoseAccess.h>

#include <filesystem>
#include <string>
#include <vector>

namespace caustica
{
namespace
{

scene::SceneEntityWorld* logicEntityWorld(App& app)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    return scene ? scene->getEntityWorld() : nullptr;
}

} // namespace

bool setEntityLocalTransform(
    App& app,
    ecs::Entity entity,
    const std::optional<math::double3>& translation,
    const std::optional<math::dquat>& rotation,
    const std::optional<math::double3>& scaling)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(entity) || !ew->world().isAlive(entity))
        return false;

    const math::double3* t = translation ? &*translation : nullptr;
    const math::dquat* r = rotation ? &*rotation : nullptr;
    const math::double3* s = scaling ? &*scaling : nullptr;
    ew->setLocalTransform(entity, t, r, s);
    ew->refreshHierarchy();
    return true;
}

bool setEntityTranslation(App& app, ecs::Entity entity, const math::double3& translation)
{
    return setEntityLocalTransform(app, entity, translation, std::nullopt, std::nullopt);
}

bool setEntityVisible(App& app, ecs::Entity entity, bool visible)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(entity))
        return false;

    auto* mesh = ew->world().tryGet<scene::MeshInstanceComponent>(entity);
    if (!mesh)
        return false;
    mesh->enabled = visible;
    return true;
}

bool setParent(App& app, ecs::Entity entity, ecs::Entity parent)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(entity) || !ew->world().isAlive(entity))
        return false;

    const ecs::Entity resolvedParent = ecs::isValid(parent) ? parent : ew->root();
    if (!ew->setParent(entity, resolvedParent))
        return false;

    ew->rebuildPathsFromRoot();
    ew->refreshHierarchy();
    return true;
}

namespace
{

ecs::Entity FindEntityByNameOrPath(scene::SceneEntityWorld& world, const std::string& name)
{
    if (name.empty())
        return ecs::NullEntity;

    const std::filesystem::path query(name);
    if (query.is_absolute())
        return world.findEntity(query);

    if (ecs::Entity found = world.findEntity(std::filesystem::path("/") / query); ecs::isValid(found))
        return found;

    ecs::Entity match = ecs::NullEntity;
    auto walk = [&](auto& self, ecs::Entity entity) -> void {
        if (!ecs::isValid(entity) || ecs::isValid(match))
            return;
        if (world.getEntityName(entity) == name)
        {
            match = entity;
            return;
        }
        for (ecs::Entity child : world.getEntityChildren(entity))
            self(self, child);
    };
    walk(walk, world.root());
    return match;
}

} // namespace

size_t setWorldPoses(
    App& app,
    const std::vector<std::string>& names,
    const float* translationsNx3,
    const float* rotationsXyzwNx4,
    const float* scalesNx3,
    std::string* firstMissingName)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew)
        return 0;
    if (names.empty())
        return 0;
    if (!translationsNx3 || !rotationsXyzwNx4)
        return 0;

    const size_t count = names.size();
    std::vector<ecs::Entity> entities(count, ecs::NullEntity);
    std::vector<scene::EntityPose> poses(count);
    size_t resolved = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const ecs::Entity entity = FindEntityByNameOrPath(*ew, names[i]);
        if (!ecs::isValid(entity))
        {
            if (firstMissingName && firstMissingName->empty())
                *firstMissingName = names[i];
            continue;
        }
        entities[resolved] = entity;
        scene::EntityPose pose;
        pose.position = math::double3(
            translationsNx3[i * 3 + 0],
            translationsNx3[i * 3 + 1],
            translationsNx3[i * 3 + 2]);
        const double xyzw[4] = {
            rotationsXyzwNx4[i * 4 + 0],
            rotationsXyzwNx4[i * 4 + 1],
            rotationsXyzwNx4[i * 4 + 2],
            rotationsXyzwNx4[i * 4 + 3],
        };
        pose.rotation = math::dquat::fromXYZW(xyzw);
        if (scalesNx3)
        {
            pose.scaling = math::double3(
                scalesNx3[i * 3 + 0],
                scalesNx3[i * 3 + 1],
                scalesNx3[i * 3 + 2]);
        }
        poses[resolved] = pose;
        ++resolved;
    }

    entities.resize(resolved);
    poses.resize(resolved);
    if (resolved == 0)
        return 0;
    if (!scene::setEntityWorldPoses(*ew, entities.data(), poses.data(), resolved))
        return 0;
    return resolved;
}

} // namespace caustica
