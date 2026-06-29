#include <scene/SceneEcs.h>
#include <scene/SceneAnimation.h>

#include <algorithm>
#include <functional>

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

void CopyEntityComponents(
    ecs::World& dstWorld,
    ecs::Entity dstEntity,
    const ecs::World& srcWorld,
    ecs::Entity srcEntity)
{
    if (const auto* name = srcWorld.get<NameComponent>(srcEntity))
        dstWorld.emplace<NameComponent>(dstEntity, *name);
    if (const auto* path = srcWorld.get<PathComponent>(srcEntity))
        dstWorld.emplace<PathComponent>(dstEntity, *path);
    if (const auto* local = srcWorld.get<LocalTransformComponent>(srcEntity))
        dstWorld.emplace<LocalTransformComponent>(dstEntity, *local);
    if (const auto* global = srcWorld.get<GlobalTransformComponent>(srcEntity))
        dstWorld.emplace<GlobalTransformComponent>(dstEntity, *global);
    if (const auto* localBounds = srcWorld.get<LocalBoundsComponent>(srcEntity))
        dstWorld.emplace<LocalBoundsComponent>(dstEntity, *localBounds);
    if (const auto* bounds = srcWorld.get<BoundsComponent>(srcEntity))
        dstWorld.emplace<BoundsComponent>(dstEntity, *bounds);
    if (const auto* content = srcWorld.get<SceneContentComponent>(srcEntity))
        dstWorld.emplace<SceneContentComponent>(dstEntity, *content);

    if (const auto* mesh = srcWorld.get<MeshInstanceComponent>(srcEntity))
        dstWorld.emplace<MeshInstanceComponent>(dstEntity, *mesh);
    if (const auto* skinned = srcWorld.get<SkinnedMeshInstanceComponent>(srcEntity))
        dstWorld.emplace<SkinnedMeshInstanceComponent>(dstEntity, *skinned);
    if (const auto* skinRef = srcWorld.get<SkinnedMeshReferenceComponent>(srcEntity))
        dstWorld.emplace<SkinnedMeshReferenceComponent>(dstEntity, *skinRef);
    if (const auto* light = srcWorld.get<LightComponent>(srcEntity))
        dstWorld.emplace<LightComponent>(dstEntity, *light);
    if (const auto* camera = srcWorld.get<CameraComponent>(srcEntity))
        dstWorld.emplace<CameraComponent>(dstEntity, *camera);
    if (const auto* animation = srcWorld.get<AnimationComponent>(srcEntity))
        dstWorld.emplace<AnimationComponent>(dstEntity, *animation);
    if (const auto* splat = srcWorld.get<GaussianSplatComponent>(srcEntity))
        dstWorld.emplace<GaussianSplatComponent>(dstEntity, *splat);
    if (const auto* sample = srcWorld.get<SampleSettingsComponent>(srcEntity))
        dstWorld.emplace<SampleSettingsComponent>(dstEntity, *sample);
    if (const auto* game = srcWorld.get<GameSettingsComponent>(srcEntity))
        dstWorld.emplace<GameSettingsComponent>(dstEntity, *game);
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
    m_Materials = {};
    m_Meshes = {};
    m_GeometryCount = 0;
    m_MaxGeometryCountPerMesh = 0;
    m_GeometryInstancesCount = 0;
    m_MeshInstances.clear();
    m_SkinnedMeshInstances.clear();
    m_Lights.clear();
    m_Cameras.clear();
    m_Animations.clear();
    m_structureDirty = true;
    m_transformDirty = true;
    m_previousTransformDirty = false;
}

void SceneEntityWorld::markStructureDirty()
{
    m_structureDirty = true;
    m_transformDirty = true;
}

void SceneEntityWorld::markTransformDirty()
{
    m_transformDirty = true;
}

void SceneEntityWorld::refreshHierarchy(PreviousTransformPolicy previousPolicy)
{
    UpdateHierarchy(m_world, previousPolicy);
}

void SceneEntityWorld::refresh(uint32_t frameIndex)
{
    const bool structureDirty = m_structureDirty;
    const bool transformDirty = m_transformDirty;

    refreshHierarchy(PreviousTransformPolicy::CaptureCurrent);

    m_world.each<LightComponent, GlobalTransformComponent>(
        [](ecs::Entity entity, LightComponent& lightComponent, GlobalTransformComponent& global) {
            lightComponent.light->ownerEntity = entity;
            lightComponent.light->cachedGlobalTransform = global.transform;
        });
    m_world.each<CameraComponent, GlobalTransformComponent>(
        [](ecs::Entity entity, CameraComponent& cameraComponent, GlobalTransformComponent& global) {
            cameraComponent.camera->ownerEntity = entity;
            cameraComponent.camera->cachedGlobalTransform = global.transform;
        });
    m_world.each<GaussianSplatComponent, GlobalTransformComponent>(
        [](ecs::Entity entity, GaussianSplatComponent& splatComponent, GlobalTransformComponent& global) {
            splatComponent.splat->ownerEntity = entity;
            splatComponent.splat->cachedGlobalTransform = global.transform;
        });

    if (transformDirty || structureDirty)
    {
        m_world.each<SkinnedMeshReferenceComponent, TransformDirtyComponent>(
            [frameIndex](ecs::Entity, SkinnedMeshReferenceComponent& ref, TransformDirtyComponent&) {
                if (auto instance = ref.reference->GetInstance())
                    instance->SetLastUpdateFrameIndex(frameIndex);
            });
    }

    if (structureDirty)
    {
        RefreshInstanceIndices();
        assignGlobalResourceIndices();
    }

    m_structureDirty = false;
    m_transformDirty = false;
    m_previousTransformDirty = structureDirty || transformDirty;
}

void SceneEntityWorld::assignGlobalResourceIndices()
{
    int meshIndex = 0;
    int geometryIndex = 0;
    for (const auto& mesh : m_Meshes)
    {
        for (const auto& geometry : mesh->geometries)
        {
            geometry->globalGeometryIndex = geometryIndex;
            ++geometryIndex;
        }
        mesh->globalMeshIndex = meshIndex;
        ++meshIndex;
    }

    int materialIndex = 0;
    for (const auto& material : m_Materials)
    {
        material->materialID = materialIndex;
        ++materialIndex;
    }
}

void SceneEntityWorld::applyAnimations(float time)
{
    for (const auto& animation : m_Animations)
        animation->Apply(time, *this);
    markTransformDirty();
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

    markStructureDirty();
    return entity;
}

void SceneEntityWorld::unregisterEntityLeaves(ecs::Entity entity)
{
    if (auto* mesh = m_world.get<MeshInstanceComponent>(entity))
    {
        if (mesh->instance)
            UnregisterLeaf(mesh->instance);
    }
    if (auto* camera = m_world.get<CameraComponent>(entity))
    {
        if (camera->camera)
            UnregisterLeaf(camera->camera);
    }
    if (auto* light = m_world.get<LightComponent>(entity))
    {
        if (light->light)
            UnregisterLeaf(light->light);
    }
    if (auto* animation = m_world.get<AnimationComponent>(entity))
    {
        if (animation->animation)
            UnregisterLeaf(animation->animation);
    }
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

    unregisterEntityLeaves(entity);

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
    markStructureDirty();
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
    markStructureDirty();
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
    markTransformDirty();
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

void SceneEntityWorld::rebuildPathsFromRoot()
{
    m_pathToEntity.clear();
    if (!ecs::isValid(m_root))
        return;

    std::function<void(ecs::Entity, const std::filesystem::path&)> visit =
        [&](ecs::Entity entity, const std::filesystem::path& parentPath) {
            std::string name = getEntityName(entity);
            std::filesystem::path path = parentPath.empty()
                ? std::filesystem::path("/") / name
                : parentPath / name;
            setPath(entity, path);

            for (ecs::Entity child : getEntityChildren(entity))
                visit(child, path);
        };

    visit(m_root, {});
}

void SceneEntityWorld::updateLeafContentAndBounds(ecs::Entity entity)
{
    SceneContentFlags leafContent = SceneContentFlags::None;
    dm::box3 localBounds = dm::box3::empty();

    if (auto* mesh = m_world.get<MeshInstanceComponent>(entity))
    {
        if (mesh->instance)
        {
            leafContent = mesh->instance->GetContentFlags();
            localBounds = mesh->instance->GetLocalBoundingBox();
        }
    }
    else if (auto* camera = m_world.get<CameraComponent>(entity))
    {
        if (camera->camera)
            leafContent = camera->camera->GetContentFlags();
    }
    else if (auto* light = m_world.get<LightComponent>(entity))
    {
        if (light->light)
            leafContent = light->light->GetContentFlags();
    }
    else if (auto* animation = m_world.get<AnimationComponent>(entity))
    {
        if (animation->animation)
            leafContent = animation->animation->GetContentFlags();
    }

    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{ localBounds });
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leafContent,
        .subgraphContent = leafContent,
    });
}

void SceneEntityWorld::setMeshInstance(ecs::Entity entity, const std::shared_ptr<MeshInstance>& instance)
{
    if (!instance)
        return;
    instance->ownerEntity = entity;
    m_world.emplace<MeshInstanceComponent>(entity, MeshInstanceComponent{ instance });
    RegisterLeaf(instance);
    updateLeafContentAndBounds(entity);
    markStructureDirty();
}

void SceneEntityWorld::setSkinnedMeshInstance(ecs::Entity entity, const std::shared_ptr<SkinnedMeshInstance>& instance)
{
    setMeshInstance(entity, instance);
    m_world.emplace<SkinnedMeshInstanceComponent>(entity, SkinnedMeshInstanceComponent{ instance });
}

void SceneEntityWorld::setSkinnedMeshReference(ecs::Entity entity, const std::shared_ptr<SkinnedMeshReference>& reference)
{
    if (!reference)
        return;
    m_world.emplace<SkinnedMeshReferenceComponent>(entity, SkinnedMeshReferenceComponent{ reference });
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setLight(ecs::Entity entity, const std::shared_ptr<Light>& light)
{
    if (!light)
        return;
    light->ownerEntity = entity;
    m_world.emplace<LightComponent>(entity, LightComponent{ light });
    RegisterLeaf(light);
    updateLeafContentAndBounds(entity);
    markStructureDirty();
}

void SceneEntityWorld::setCamera(ecs::Entity entity, const std::shared_ptr<SceneCamera>& camera)
{
    if (!camera)
        return;
    camera->ownerEntity = entity;
    m_world.emplace<CameraComponent>(entity, CameraComponent{ camera });
    RegisterLeaf(camera);
    updateLeafContentAndBounds(entity);
    markStructureDirty();
}

void SceneEntityWorld::setAnimation(ecs::Entity entity, const std::shared_ptr<SceneAnimation>& animation)
{
    if (!animation)
        return;
    m_world.emplace<AnimationComponent>(entity, AnimationComponent{ animation });
    RegisterLeaf(animation);
    updateLeafContentAndBounds(entity);
    markStructureDirty();
}

void SceneEntityWorld::setGaussianSplat(ecs::Entity entity, const std::shared_ptr<GaussianSplat>& splat)
{
    if (!splat)
        return;
    splat->ownerEntity = entity;
    m_world.emplace<GaussianSplatComponent>(entity, GaussianSplatComponent{ splat });
    updateLeafContentAndBounds(entity);
    markStructureDirty();
}

void SceneEntityWorld::setSampleSettings(ecs::Entity entity, const std::shared_ptr<SampleSettings>& settings)
{
    if (!settings)
        return;
    m_world.emplace<SampleSettingsComponent>(entity, SampleSettingsComponent{ settings });
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setGameSettings(ecs::Entity entity, const std::shared_ptr<GameSettings>& settings)
{
    if (!settings)
        return;
    m_world.emplace<GameSettingsComponent>(entity, GameSettingsComponent{ settings });
    updateLeafContentAndBounds(entity);
}

ecs::Entity SceneEntityWorld::importSubtree(ecs::Entity parent, const SceneEntityWorld& source, ecs::Entity sourceRoot)
{
    if (!ecs::isValid(sourceRoot))
        return ecs::NullEntity;

    std::unordered_map<ecs::Entity, ecs::Entity> entityMap;
    ecs::Entity newRoot = ecs::NullEntity;

    std::function<void(ecs::Entity, ecs::Entity)> copyRecursive =
        [&](ecs::Entity srcEntity, ecs::Entity dstParent) {
            const auto* srcName = source.m_world.get<NameComponent>(srcEntity);
            const std::string name = srcName ? srcName->value : std::string{};
            ecs::Entity dstEntity = createEntity(name, dstParent);
            entityMap[srcEntity] = dstEntity;

            if (srcEntity == sourceRoot)
                newRoot = dstEntity;

            CopyEntityComponents(m_world, dstEntity, source.m_world, srcEntity);

            if (auto* mesh = m_world.get<MeshInstanceComponent>(dstEntity))
            {
                if (mesh->instance)
                {
                    mesh->instance->ownerEntity = dstEntity;
                    RegisterLeaf(mesh->instance);
                }
            }
            if (auto* light = m_world.get<LightComponent>(dstEntity))
            {
                if (light->light)
                    RegisterLeaf(light->light);
            }
            if (auto* camera = m_world.get<CameraComponent>(dstEntity))
            {
                if (camera->camera)
                    RegisterLeaf(camera->camera);
            }
            if (auto* animation = m_world.get<AnimationComponent>(dstEntity))
            {
                if (animation->animation)
                    RegisterLeaf(animation->animation);
            }

            if (const auto* children = source.m_world.get<ChildrenComponent>(srcEntity))
            {
                for (ecs::Entity srcChild : children->children)
                    copyRecursive(srcChild, dstEntity);
            }
        };

    copyRecursive(sourceRoot, parent);

    for (const auto& skinned : m_SkinnedMeshInstances)
    {
        for (auto& joint : skinned->joints)
        {
            if (ecs::isValid(joint.jointEntity))
            {
                auto it = entityMap.find(joint.jointEntity);
                if (it != entityMap.end())
                    joint.jointEntity = it->second;
            }
        }
    }

    for (const auto& animation : m_Animations)
    {
        for (const auto& channel : animation->GetChannels())
        {
            ecs::Entity target = channel->GetTargetEntity();
            if (ecs::isValid(target))
            {
                auto it = entityMap.find(target);
                if (it != entityMap.end())
                    channel->SetTargetEntity(it->second);
            }
        }
    }

    rebuildPathsFromRoot();
    markStructureDirty();
    return newRoot;
}

ecs::Entity SceneEntityWorld::entityForPath(const std::filesystem::path& path) const
{
    auto it = m_pathToEntity.find(path.generic_string());
    return it == m_pathToEntity.end() ? ecs::NullEntity : it->second;
}

ecs::Entity SceneEntityWorld::findEntity(const std::filesystem::path& path, ecs::Entity context) const
{
    auto pathComponent = path.begin();
    if (pathComponent == path.end())
        return ecs::NullEntity;

    if (*pathComponent == "/")
    {
        context = m_root;
        ++pathComponent;
    }

    if (!ecs::isValid(context))
        return ecs::NullEntity;

    ecs::Entity current = context;

    while (ecs::isValid(current) && pathComponent != path.end())
    {
        if (*pathComponent == "..")
        {
            if (const auto* parent = m_world.get<ParentComponent>(current))
                current = parent->parent;
            else
                current = ecs::NullEntity;
            ++pathComponent;
            continue;
        }

        ecs::Entity found = ecs::NullEntity;
        if (const auto* children = m_world.get<ChildrenComponent>(current))
        {
            for (ecs::Entity child : children->children)
            {
                if (getEntityName(child) == pathComponent->generic_string())
                {
                    found = child;
                    break;
                }
            }
        }

        if (!ecs::isValid(found))
            return ecs::NullEntity;

        current = found;
        ++pathComponent;
    }

    return current;
}

std::filesystem::path SceneEntityWorld::getEntityPath(ecs::Entity entity) const
{
    if (const auto* path = m_world.get<PathComponent>(entity))
        return path->value;
    return {};
}

std::string SceneEntityWorld::getEntityName(ecs::Entity entity) const
{
    if (const auto* name = m_world.get<NameComponent>(entity))
        return name->value;
    return {};
}

const std::vector<ecs::Entity>& SceneEntityWorld::getEntityChildren(ecs::Entity entity) const
{
    if (const auto* children = m_world.get<ChildrenComponent>(entity))
        return children->children;
    return s_emptyChildren;
}

bool SceneEntityWorld::entitySubtreeContains(ecs::Entity root, ecs::Entity candidate) const
{
    if (!ecs::isValid(root) || !ecs::isValid(candidate))
        return false;
    if (root == candidate)
        return true;

    std::vector<ecs::Entity> stack;
    for (ecs::Entity child : getEntityChildren(root))
        stack.push_back(child);

    while (!stack.empty())
    {
        const ecs::Entity entity = stack.back();
        stack.pop_back();
        if (entity == candidate)
            return true;
        for (ecs::Entity child : getEntityChildren(entity))
            stack.push_back(child);
    }

    return false;
}

} // namespace caustica::scene
