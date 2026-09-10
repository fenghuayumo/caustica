#include <engine/SceneRobot.h>

#include <engine/App.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneSpawn.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/ScenePoseAccess.h>

#include <cmath>
#include <string>
#include <string_view>
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

scene::RobotComponent* RequireRobot(scene::SceneEntityWorld& world, ecs::Entity robot)
{
    if (!ecs::isValid(robot) || !world.world().isAlive(robot))
        return nullptr;
    return world.world().tryGet<scene::RobotComponent>(robot);
}

ecs::Entity FindNamedInSubtree(
    scene::SceneEntityWorld& world,
    ecs::Entity root,
    std::string_view name)
{
    ecs::Entity found = ecs::NullEntity;
    auto walk = [&](auto& self, ecs::Entity entity) -> void {
        if (!ecs::isValid(entity) || ecs::isValid(found))
            return;
        if (world.getEntityName(entity) == name)
        {
            found = entity;
            return;
        }
        for (ecs::Entity child : world.getEntityChildren(entity))
            self(self, child);
    };
    walk(walk, root);
    return found;
}

ecs::Entity ResolveJointChild(
    scene::SceneEntityWorld& world,
    ecs::Entity robot,
    scene::RobotJointDesc& joint)
{
    if (ecs::isValid(joint.childEntity) && world.world().isAlive(joint.childEntity))
        return joint.childEntity;
    const ecs::Entity found = FindNamedInSubtree(world, robot, joint.childLink);
    joint.childEntity = found;
    return found;
}

bool ApplyJointPosition(
    scene::SceneEntityWorld& world,
    ecs::Entity robot,
    scene::RobotJointDesc& joint,
    float q)
{
    const ecs::Entity child = ResolveJointChild(world, robot, joint);
    if (!ecs::isValid(child))
        return false;

    math::double3 axis = joint.axis;
    const double axisLength = math::length(axis);
    if (axisLength > 1e-12)
        axis /= axisLength;
    else
        axis = math::double3(1.0, 0.0, 0.0);

    if (joint.type == scene::RobotJointType::Prismatic)
    {
        const math::double3 translation =
            joint.originTranslation + math::applyQuat(joint.originRotation, axis * double(q));
        world.setLocalTransform(child, &translation, &joint.originRotation, nullptr);
        return true;
    }

    const double half = 0.5 * double(q);
    const math::double3 imag = axis * std::sin(half);
    const math::dquat motion = math::dquat::fromWXYZ(std::cos(half), imag);
    const math::dquat rotation = joint.originRotation * motion;
    world.setLocalTransform(child, &joint.originTranslation, &rotation, nullptr);
    return true;
}

} // namespace

std::vector<std::string> jointNames(App& app, ecs::Entity robot)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew)
        return {};
    const scene::RobotComponent* component = RequireRobot(*ew, robot);
    if (!component)
        return {};

    std::vector<std::string> names;
    names.reserve(component->joints.size());
    for (const scene::RobotJointDesc& joint : component->joints)
    {
        if (scene::isMovableRobotJoint(joint.type) && !joint.name.empty())
            names.push_back(joint.name);
    }
    return names;
}

bool setJointPositions(App& app, ecs::Entity robot, const float* positions, size_t count)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew)
        return false;
    scene::RobotComponent* component = RequireRobot(*ew, robot);
    if (!component)
        return false;
    if (!positions && count != 0)
        return false;

    size_t movable = 0;
    for (const scene::RobotJointDesc& joint : component->joints)
    {
        if (scene::isMovableRobotJoint(joint.type) && !joint.name.empty())
            ++movable;
    }
    if (count != movable)
        return false;

    size_t cursor = 0;
    for (scene::RobotJointDesc& joint : component->joints)
    {
        if (!scene::isMovableRobotJoint(joint.type) || joint.name.empty())
            continue;
        if (!std::isfinite(positions[cursor]))
            return false;
        if (!ApplyJointPosition(*ew, robot, joint, positions[cursor]))
            return false;
        ++cursor;
    }

    ew->refreshHierarchy();
    return true;
}

bool getLinkPose(
    App& app,
    ecs::Entity robot,
    std::string_view linkName,
    math::double3& translation,
    math::dquat& rotation)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(robot) || !ew->world().isAlive(robot) || linkName.empty())
        return false;
    if (!RequireRobot(*ew, robot))
        return false;

    const ecs::Entity link = FindNamedInSubtree(*ew, robot, linkName);
    if (!ecs::isValid(link))
        return false;

    ew->refreshHierarchy();

    scene::EntityPose pose;
    if (!scene::getEntityWorldPose(*ew, link, pose))
        return false;
    translation = pose.position;
    rotation = pose.rotation;
    return true;
}

ecs::Entity attachCamera(
    App& app,
    ecs::Entity robot,
    const std::string& name,
    const std::string& link,
    const math::double3& localTranslation,
    const math::dquat& localRotation)
{
    scene::SceneEntityWorld* ew = logicEntityWorld(app);
    if (!ew || !ecs::isValid(robot) || !ew->world().isAlive(robot) || link.empty())
        return ecs::NullEntity;
    if (!RequireRobot(*ew, robot))
        return ecs::NullEntity;

    const ecs::Entity parent = FindNamedInSubtree(*ew, robot, link);
    if (!ecs::isValid(parent))
        return ecs::NullEntity;

    SpawnCameraDesc desc;
    desc.name = name;
    desc.parent = parent;
    desc.localTranslation = localTranslation;
    desc.localRotation = localRotation;
    return spawnCamera(app, std::move(desc));
}

} // namespace caustica
