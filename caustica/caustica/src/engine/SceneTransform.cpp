#include <engine/SceneTransform.h>

#include <engine/App.h>
#include <engine/SceneQuery.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>

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
    const std::optional<dm::double3>& translation,
    const std::optional<dm::dquat>& rotation,
    const std::optional<dm::double3>& scaling)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(entity) || !ew->world().isAlive(entity))
        return false;

    const dm::double3* t = translation ? &*translation : nullptr;
    const dm::dquat* r = rotation ? &*rotation : nullptr;
    const dm::double3* s = scaling ? &*scaling : nullptr;
    ew->setLocalTransform(entity, t, r, s);
    ew->refreshHierarchy();
    return true;
}

bool setEntityTranslation(App& app, ecs::Entity entity, const dm::double3& translation)
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

} // namespace caustica
