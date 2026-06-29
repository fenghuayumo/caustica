#include <scene/SceneEcs.h>

#include <algorithm>

namespace caustica::scene
{

namespace
{
dm::daffine3 ComposeLocalTransform(const LocalTransformComponent& local)
{
    dm::daffine3 transform = dm::scaling(local.scaling);
    transform *= local.rotation.toAffine();
    transform *= dm::translation(local.translation);
    return transform;
}

void RemoveChildReference(ecs::World& world, ecs::Entity parent, ecs::Entity child)
{
    if (!ecs::isValid(parent))
        return;

    auto* children = world.get<ChildrenComponent>(parent);
    if (!children)
        return;

    auto it = std::remove(children->children.begin(), children->children.end(), child);
    children->children.erase(it, children->children.end());
}

bool IsDescendantOf(const ecs::World& world, ecs::Entity candidate, ecs::Entity possibleAncestor)
{
    ecs::Entity current = candidate;
    while (ecs::isValid(current))
    {
        if (current == possibleAncestor)
            return true;

        const auto* parent = world.get<ParentComponent>(current);
        current = parent ? parent->parent : ecs::NullEntity;
    }
    return false;
}

SceneContentFlags GetLeafContent(ecs::World& world, ecs::Entity entity)
{
    const auto* content = world.get<SceneContentComponent>(entity);
    return content ? content->leafContent : SceneContentFlags::None;
}

dm::box3 GetLeafBounds(ecs::World& world, ecs::Entity entity, const dm::affine3& globalTransform)
{
    const auto* localBounds = world.get<LocalBoundsComponent>(entity);
    if (!localBounds || localBounds->bounds.isempty())
        return dm::box3::empty();

    return localBounds->bounds * globalTransform;
}

void RefreshEntityHierarchy(
    ecs::World& world,
    ecs::Entity entity,
    const dm::daffine3* parentGlobal,
    PreviousTransformPolicy previousPolicy)
{
    auto* local = world.get<LocalTransformComponent>(entity);
    auto* global = world.get<GlobalTransformComponent>(entity);
    if (!local || !global)
        return;

    local->transform = ComposeLocalTransform(*local);

    if (previousPolicy == PreviousTransformPolicy::CaptureCurrent)
    {
        global->previousTransform = global->transform;
        global->previousTransformFloat = global->transformFloat;
    }

    if (parentGlobal)
    {
        global->transform = local->hasLocalTransform
            ? local->transform * *parentGlobal
            : *parentGlobal;
    }
    else
    {
        global->transform = local->transform;
    }

    global->transformFloat = dm::affine3(global->transform);

    dm::box3 subgraphBounds = GetLeafBounds(world, entity, global->transformFloat);
    SceneContentFlags leafContent = GetLeafContent(world, entity);
    SceneContentFlags subgraphContent = leafContent;

    if (auto* children = world.get<ChildrenComponent>(entity))
    {
        for (ecs::Entity child : children->children)
        {
            if (!world.isAlive(child))
                continue;

            RefreshEntityHierarchy(world, child, &global->transform, previousPolicy);

            if (auto* childBounds = world.get<BoundsComponent>(child))
                subgraphBounds |= childBounds->globalBounds;
            if (auto* childContent = world.get<SceneContentComponent>(child))
                subgraphContent |= childContent->subgraphContent;
        }
    }

    world.emplace<BoundsComponent>(entity, BoundsComponent{ subgraphBounds });
    world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leafContent,
        .subgraphContent = subgraphContent,
    });

    world.remove<TransformDirtyComponent>(entity);
    world.remove<HierarchyDirtyComponent>(entity);
}
} // namespace

void UpdateHierarchy(ecs::World& world, PreviousTransformPolicy previousPolicy)
{
    if (const auto* root = world.getResource<SceneRootResource>();
        root && ecs::isValid(root->root) && world.isAlive(root->root))
    {
        RefreshEntityHierarchy(world, root->root, nullptr, previousPolicy);
        return;
    }

    world.each<LocalTransformComponent, GlobalTransformComponent>(
        [&world, previousPolicy](ecs::Entity entity, LocalTransformComponent&, GlobalTransformComponent&) {
            if (!world.has<ParentComponent>(entity))
                RefreshEntityHierarchy(world, entity, nullptr, previousPolicy);
        });
}

void SceneEntityWorld::clear()
{
    m_world.clear();
    m_root = ecs::NullEntity;
    m_pathToEntity.clear();
}

void SceneEntityWorld::refreshHierarchy(PreviousTransformPolicy previousPolicy)
{
    UpdateHierarchy(m_world, previousPolicy);
}

ecs::Entity SceneEntityWorld::createEntity(const std::string& name, ecs::Entity parent)
{
    ecs::Entity entity = m_world.spawn();
    m_world.emplace<NameComponent>(entity, NameComponent{ name });
    m_world.emplace<ChildrenComponent>(entity, ChildrenComponent{});
    m_world.emplace<LocalTransformComponent>(entity, LocalTransformComponent{});
    m_world.emplace<GlobalTransformComponent>(entity, GlobalTransformComponent{});
    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{});
    m_world.emplace<BoundsComponent>(entity, BoundsComponent{});
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{});
    m_world.emplace<TransformDirtyComponent>(entity, TransformDirtyComponent{});
    m_world.emplace<HierarchyDirtyComponent>(entity, HierarchyDirtyComponent{});

    if (!ecs::isValid(m_root))
    {
        m_root = entity;
        m_world.insertResource<SceneRootResource>(SceneRootResource{ m_root });
    }

    if (ecs::isValid(parent))
        setParent(entity, parent);

    return entity;
}

void SceneEntityWorld::destroyEntity(ecs::Entity entity)
{
    if (!m_world.isAlive(entity))
        return;

    if (auto* children = m_world.get<ChildrenComponent>(entity))
    {
        auto childCopy = children->children;
        for (ecs::Entity child : childCopy)
            destroyEntity(child);
    }

    if (auto* parent = m_world.get<ParentComponent>(entity))
        RemoveChildReference(m_world, parent->parent, entity);

    if (auto* path = m_world.get<PathComponent>(entity))
        m_pathToEntity.erase(path->value.generic_string());

    if (entity == m_root)
    {
        m_root = ecs::NullEntity;
        if (auto* root = m_world.getResource<SceneRootResource>())
            root->root = ecs::NullEntity;
    }

    m_world.despawn(entity);
}

bool SceneEntityWorld::setParent(ecs::Entity entity, ecs::Entity parent)
{
    if (!m_world.isAlive(entity))
        return false;
    if (ecs::isValid(parent) && (!m_world.isAlive(parent) || parent == entity || IsDescendantOf(m_world, parent, entity)))
        return false;

    ecs::Entity oldParent = ecs::NullEntity;
    if (auto* oldParentComponent = m_world.get<ParentComponent>(entity))
        oldParent = oldParentComponent->parent;

    if (oldParent == parent)
        return true;

    RemoveChildReference(m_world, oldParent, entity);

    if (ecs::isValid(parent))
    {
        m_world.emplace<ParentComponent>(entity, ParentComponent{ parent });
        auto* children = m_world.get<ChildrenComponent>(parent);
        if (!children)
            children = &m_world.emplace<ChildrenComponent>(parent, ChildrenComponent{});
        if (std::find(children->children.begin(), children->children.end(), entity) == children->children.end())
            children->children.push_back(entity);
    }
    else
    {
        m_world.remove<ParentComponent>(entity);
    }

    m_world.emplace<HierarchyDirtyComponent>(entity, HierarchyDirtyComponent{});
    return true;
}

void SceneEntityWorld::setLocalTransform(
    ecs::Entity entity,
    const dm::double3* translation,
    const dm::dquat* rotation,
    const dm::double3* scaling)
{
    if (!m_world.isAlive(entity))
        return;

    auto* local = m_world.get<LocalTransformComponent>(entity);
    if (!local)
        local = &m_world.emplace<LocalTransformComponent>(entity, LocalTransformComponent{});

    if (translation)
        local->translation = *translation;
    if (rotation)
        local->rotation = *rotation;
    if (scaling)
        local->scaling = *scaling;

    local->hasLocalTransform = true;
    local->transform = ComposeLocalTransform(*local);
    m_world.emplace<TransformDirtyComponent>(entity, TransformDirtyComponent{});
}

void SceneEntityWorld::setTranslation(ecs::Entity entity, const dm::double3& translation)
{
    setLocalTransform(entity, &translation, nullptr, nullptr);
}

void SceneEntityWorld::setRotation(ecs::Entity entity, const dm::dquat& rotation)
{
    setLocalTransform(entity, nullptr, &rotation, nullptr);
}

void SceneEntityWorld::setScaling(ecs::Entity entity, const dm::double3& scaling)
{
    setLocalTransform(entity, nullptr, nullptr, &scaling);
}

void SceneEntityWorld::setPath(ecs::Entity entity, const std::filesystem::path& path)
{
    if (!m_world.isAlive(entity))
        return;

    if (auto* current = m_world.get<PathComponent>(entity))
        m_pathToEntity.erase(current->value.generic_string());

    m_world.emplace<PathComponent>(entity, PathComponent{ path });
    m_pathToEntity[path.generic_string()] = entity;
}

ecs::Entity SceneEntityWorld::entityForPath(const std::filesystem::path& path) const
{
    auto it = m_pathToEntity.find(path.generic_string());
    return it == m_pathToEntity.end() ? ecs::NullEntity : it->second;
}

} // namespace caustica::scene
