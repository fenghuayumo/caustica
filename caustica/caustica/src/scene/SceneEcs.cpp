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

LocalTransformComponent MakeLocalTransform(const SceneGraphNode& node)
{
    return LocalTransformComponent{
        .translation = node.GetTranslation(),
        .rotation = node.GetRotation(),
        .scaling = node.GetScaling(),
        .transform = node.GetLocalToParentTransform(),
        .hasLocalTransform = true,
    };
}

GlobalTransformComponent MakeGlobalTransform(const SceneGraphNode& node)
{
    return GlobalTransformComponent{
        .transform = node.GetLocalToWorldTransform(),
        .transformFloat = node.GetLocalToWorldTransformFloat(),
        .previousTransform = node.GetPrevLocalToWorldTransform(),
        .previousTransformFloat = node.GetPrevLocalToWorldTransformFloat(),
    };
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
    m_nodeToEntity.clear();
    m_pathToEntity.clear();
}

void SceneEntityWorld::rebuildFromSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph)
{
    clear();

    if (!sceneGraph || !sceneGraph->GetRootNode())
        return;

    m_root = importNode(sceneGraph->GetRootNode(), ecs::NullEntity);
    m_world.insertResource<SceneRootResource>(SceneRootResource{ m_root });
    refreshHierarchy(PreviousTransformPolicy::PreserveExisting);
}

void SceneEntityWorld::syncTransformsFromSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph)
{
    if (!sceneGraph || !sceneGraph->GetRootNode())
    {
        clear();
        return;
    }

    if (!ecs::isValid(m_root) || m_nodeToEntity.empty())
    {
        rebuildFromSceneGraph(sceneGraph);
        return;
    }

    syncNodeRecursive(sceneGraph->GetRootNode());
    refreshHierarchy(PreviousTransformPolicy::PreserveExisting);
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

    if (auto* node = m_world.get<LegacySceneNodeComponent>(entity))
    {
        if (auto sharedNode = node->node.lock())
        {
            m_nodeToEntity.erase(sharedNode.get());
            m_pathToEntity.erase(sharedNode->GetPath().generic_string());
        }
    }

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

ecs::Entity SceneEntityWorld::entityForNode(const SceneGraphNode* node) const
{
    auto it = m_nodeToEntity.find(node);
    return it == m_nodeToEntity.end() ? ecs::NullEntity : it->second;
}

std::shared_ptr<SceneGraphNode> SceneEntityWorld::nodeForEntity(ecs::Entity entity) const
{
    if (!ecs::isValid(entity))
        return nullptr;

    const auto* component = m_world.get<LegacySceneNodeComponent>(entity);
    return component ? component->node.lock() : nullptr;
}

ecs::Entity SceneEntityWorld::entityForPath(const std::filesystem::path& path) const
{
    auto it = m_pathToEntity.find(path.generic_string());
    return it == m_pathToEntity.end() ? ecs::NullEntity : it->second;
}

ecs::Entity SceneEntityWorld::importNode(const std::shared_ptr<SceneGraphNode>& node, ecs::Entity parent)
{
    ecs::Entity entity = m_world.spawn();
    m_nodeToEntity[node.get()] = entity;
    m_pathToEntity[node->GetPath().generic_string()] = entity;

    m_world.emplace<NameComponent>(entity, NameComponent{ node->GetName() });
    m_world.emplace<PathComponent>(entity, PathComponent{ node->GetPath() });
    m_world.emplace<ChildrenComponent>(entity, ChildrenComponent{});
    m_world.emplace<LocalTransformComponent>(entity, MakeLocalTransform(*node));
    m_world.emplace<GlobalTransformComponent>(entity, MakeGlobalTransform(*node));
    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{});
    m_world.emplace<BoundsComponent>(entity, BoundsComponent{ node->GetGlobalBoundingBox() });
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = node->GetLeafContentFlags(),
        .subgraphContent = node->GetSubgraphContentFlags(),
    });
    m_world.emplace<LegacySceneNodeComponent>(entity, LegacySceneNodeComponent{ node });

    if (ecs::isValid(parent))
    {
        m_world.emplace<ParentComponent>(entity, ParentComponent{ parent });
        if (auto* children = m_world.get<ChildrenComponent>(parent))
            children->children.push_back(entity);
    }

    attachLegacyLeafComponents(entity, node->GetLeaf());

    for (size_t index = 0; index < node->GetNumChildren(); ++index)
        importNode(node->GetChild(index)->shared_from_this(), entity);

    return entity;
}

void SceneEntityWorld::syncNodeRecursive(const std::shared_ptr<SceneGraphNode>& node)
{
    const ecs::Entity entity = entityForNode(node.get());
    if (!ecs::isValid(entity))
    {
        rebuildFromSceneGraph(node->GetGraph());
        return;
    }

    if (auto* name = m_world.get<NameComponent>(entity))
        name->value = node->GetName();
    if (auto* path = m_world.get<PathComponent>(entity))
    {
        m_pathToEntity.erase(path->value.generic_string());
        path->value = node->GetPath();
        m_pathToEntity[path->value.generic_string()] = entity;
    }
    if (auto* local = m_world.get<LocalTransformComponent>(entity))
        *local = MakeLocalTransform(*node);
    if (auto* global = m_world.get<GlobalTransformComponent>(entity))
        *global = MakeGlobalTransform(*node);
    if (auto* bounds = m_world.get<BoundsComponent>(entity))
        bounds->globalBounds = node->GetGlobalBoundingBox();
    if (auto* content = m_world.get<SceneContentComponent>(entity))
    {
        content->leafContent = node->GetLeafContentFlags();
        content->subgraphContent = node->GetSubgraphContentFlags();
    }
    const auto* children = m_world.get<ChildrenComponent>(entity);
    if (!children || children->children.size() != node->GetNumChildren())
    {
        rebuildFromSceneGraph(node->GetGraph());
        return;
    }

    for (size_t index = 0; index < node->GetNumChildren(); ++index)
        syncNodeRecursive(node->GetChild(index)->shared_from_this());
}

void SceneEntityWorld::attachLegacyLeafComponents(ecs::Entity entity, const std::shared_ptr<SceneGraphLeaf>& leaf)
{
    if (!leaf)
        return;

    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{ leaf->GetLocalBoundingBox() });
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leaf->GetContentFlags(),
        .subgraphContent = leaf->GetContentFlags(),
    });

    if (auto mesh = std::dynamic_pointer_cast<MeshInstance>(leaf))
        m_world.emplace<MeshInstanceComponent>(entity, MeshInstanceComponent{ mesh });
    if (auto skinnedMesh = std::dynamic_pointer_cast<SkinnedMeshInstance>(leaf))
        m_world.emplace<SkinnedMeshInstanceComponent>(entity, SkinnedMeshInstanceComponent{ skinnedMesh });
    if (auto light = std::dynamic_pointer_cast<Light>(leaf))
        m_world.emplace<LightComponent>(entity, LightComponent{ light });
    if (auto camera = std::dynamic_pointer_cast<SceneCamera>(leaf))
        m_world.emplace<CameraComponent>(entity, CameraComponent{ camera });
    if (auto animation = std::dynamic_pointer_cast<SceneGraphAnimation>(leaf))
        m_world.emplace<AnimationComponent>(entity, AnimationComponent{ animation });
    if (auto splat = std::dynamic_pointer_cast<GaussianSplat>(leaf))
        m_world.emplace<GaussianSplatComponent>(entity, GaussianSplatComponent{ splat });
    if (auto sampleSettings = std::dynamic_pointer_cast<SampleSettings>(leaf))
        m_world.emplace<SampleSettingsComponent>(entity, SampleSettingsComponent{ sampleSettings });
    if (auto gameSettings = std::dynamic_pointer_cast<GameSettings>(leaf))
        m_world.emplace<GameSettingsComponent>(entity, GameSettingsComponent{ gameSettings });
}

} // namespace caustica::scene
