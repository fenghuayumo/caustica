#include <scene/SceneEcs.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneMeshAccess.h>

#include <ecs/ChangeDetection.h>

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

    world.registry().emplace_or_replace<BoundsComponent>(entity, BoundsComponent{ subgraphBounds });
    world.registry().emplace_or_replace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leafContent,
        .subgraphContent = subgraphContent,
    });
}

void CopyEntityComponents(
    ecs::World& dstWorld,
    ecs::Entity dstEntity,
    const ecs::World& srcWorld,
    ecs::Entity srcEntity,
    bool copyMeshComponents = true)
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

    if (copyMeshComponents)
    {
        if (const auto* mesh = srcWorld.get<MeshInstanceComponent>(srcEntity))
            dstWorld.emplace<MeshInstanceComponent>(dstEntity, *mesh);
        if (const auto* skinned = srcWorld.get<SkinnedMeshComponent>(srcEntity))
            dstWorld.emplace<SkinnedMeshComponent>(dstEntity, *skinned);
        if (const auto* skinnedGpu = srcWorld.get<SkinnedMeshGpuComponent>(srcEntity))
            dstWorld.emplace<SkinnedMeshGpuComponent>(dstEntity, *skinnedGpu);
    }
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

    world.each<LocalTransformComponent, GlobalTransformComponent, ecs::Without<ParentComponent>>(
        [&world, previousPolicy](ecs::Entity entity, LocalTransformComponent&, GlobalTransformComponent&) {
            RefreshEntityHierarchy(world, entity, nullptr, previousPolicy);
        });
}

void SceneEntityWorld::registerCameraEntity(ecs::Entity entity)
{
    if (!m_world.isAlive(entity) || !m_world.has<CameraComponent>(entity))
        return;

    if (std::find(m_CameraEntities.begin(), m_CameraEntities.end(), entity) != m_CameraEntities.end())
        return;

    m_CameraEntities.push_back(entity);
}

void SceneEntityWorld::unregisterCameraEntity(ecs::Entity entity)
{
    const auto it = std::find(m_CameraEntities.begin(), m_CameraEntities.end(), entity);
    if (it != m_CameraEntities.end())
        m_CameraEntities.erase(it);
}

void SceneEntityWorld::clear()
{
    m_world.clear();
    m_root = ecs::NullEntity;
    m_CameraEntities.clear();
    m_pathToEntity.clear();
    m_Materials = {};
    m_Meshes = {};
    m_GeometryCount = 0;
    m_MaxGeometryCountPerMesh = 0;
    m_GeometryInstancesCount = 0;
    m_structureDirty = true;
    m_transformDirty = true;
    m_previousTransformDirty = false;
    ensureChangeDetection();
}

void SceneEntityWorld::ensureChangeDetection()
{
    m_world.enableChangeDetection();
}

void SceneEntityWorld::syncDirtyFlagsFromChangeDetection()
{
    const auto* changeDetection = m_world.getResource<ecs::ChangeDetection>();
    if (!changeDetection)
        return;

    const auto& registry = m_world.registry();

    if (changeDetection->worldStructureChanged()
        || changeDetection->anyOfChangedThisFrame<
            MeshInstanceComponent,
            SkinnedMeshComponent,
            LightComponent,
            CameraComponent,
            AnimationComponent,
            GaussianSplatComponent,
            ParentComponent,
            ChildrenComponent>(registry)
        || changeDetection->anyOfAddedThisFrame<
            MeshInstanceComponent,
            SkinnedMeshComponent,
            LightComponent,
            CameraComponent,
            AnimationComponent,
            GaussianSplatComponent,
            ParentComponent,
            ChildrenComponent>(registry))
    {
        m_structureDirty = true;
        m_transformDirty = true;
    }

    if (changeDetection->anyOfChangedThisFrame<LocalTransformComponent, ParentComponent>(registry)
        || changeDetection->anyOfAddedThisFrame<LocalTransformComponent, ParentComponent>(registry))
    {
        m_transformDirty = true;
    }
}

void SceneEntityWorld::refreshHierarchy(PreviousTransformPolicy previousPolicy)
{
    UpdateHierarchy(m_world, previousPolicy);
}

void SceneEntityWorld::ensureScheduleBuilt()
{
    if (m_scheduleBuilt)
        return;
    m_scheduleBuilt = true;

    // "Hierarchy" runs first and produces GlobalTransformComponent for everything below it.
    m_schedule.addSet("Hierarchy");
    m_schedule.addSystem("Hierarchy", "RefreshHierarchy",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemRefreshHierarchy(world, ctx); });

    // "PostHierarchy" runs in registration order; FinalizeFrameFlags must stay last.
    m_schedule.addSet("PostHierarchy");
    m_schedule.addSystem("PostHierarchy", "UpdateGaussianSplatTransforms",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemUpdateGaussianSplatTransforms(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "MarkDirtySkinnedMeshesFromChangedJoints",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemMarkDirtySkinnedMeshesFromChangedJoints(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "MarkDirtySkinnedMeshes",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemMarkDirtySkinnedMeshes(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "RefreshInstanceIndices",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemRefreshInstanceIndices(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "AssignGlobalResourceIndices",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemAssignGlobalResourceIndices(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "ApplyDeferredCommands",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemApplyDeferredCommands(world, ctx); });
    m_schedule.addSystem("PostHierarchy", "FinalizeFrameFlags",
        [this](ecs::World& world, const ecs::ScheduleContext& ctx) { systemFinalizeFrameFlags(world, ctx); });

    m_schedule.before("RefreshHierarchy", "UpdateGaussianSplatTransforms");
    m_schedule.before("RefreshInstanceIndices", "AssignGlobalResourceIndices");
    m_schedule.before("AssignGlobalResourceIndices", "ApplyDeferredCommands");
    m_schedule.before("ApplyDeferredCommands", "FinalizeFrameFlags");
}

void SceneEntityWorld::refresh(uint32_t frameIndex)
{
    ensureScheduleBuilt();
    ensureChangeDetection();
    syncDirtyFlagsFromChangeDetection();

    m_frameStructureDirty = m_structureDirty;
    m_frameTransformDirty = m_transformDirty;

    ecs::ScheduleContext ctx{};
    ctx.frameIndex = frameIndex;
    m_schedule.run(m_world, ctx);
}

void SceneEntityWorld::systemRefreshHierarchy(ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/)
{
    refreshHierarchy(PreviousTransformPolicy::CaptureCurrent);
}

void SceneEntityWorld::systemUpdateGaussianSplatTransforms(ecs::World& world, const ecs::ScheduleContext& /*ctx*/)
{
    world.each<GaussianSplatComponent, GlobalTransformComponent>(
        [](ecs::Entity entity, GaussianSplatComponent& splatComponent, GlobalTransformComponent& global) {
            splatComponent.splat->ownerEntity = entity;
            splatComponent.splat->cachedGlobalTransform = global.transform;
        });
}

void SceneEntityWorld::systemMarkDirtySkinnedMeshes(ecs::World& world, const ecs::ScheduleContext& ctx)
{
    if (!(m_frameTransformDirty || m_frameStructureDirty))
        return;

    const uint32_t frameIndex = ctx.frameIndex;
    world.each<SkinnedMeshReferenceComponent>(
        [frameIndex, &world](ecs::Entity, SkinnedMeshReferenceComponent& ref) {
            if (!ecs::isValid(ref.skinnedMeshEntity))
                return;
            if (auto* skinned = world.get<SkinnedMeshComponent>(ref.skinnedMeshEntity))
                skinned->lastUpdateFrameIndex = frameIndex;
        });
}

void SceneEntityWorld::systemMarkDirtySkinnedMeshesFromChangedJoints(ecs::World& world, const ecs::ScheduleContext& ctx)
{
    const uint32_t frameIndex = ctx.frameIndex;

    world.each<SkinnedMeshReferenceComponent, ecs::Changed<LocalTransformComponent>>(
        [&](ecs::Entity, SkinnedMeshReferenceComponent& ref, LocalTransformComponent&) {
            if (!ecs::isValid(ref.skinnedMeshEntity))
                return;
            if (auto* skinned = world.get<SkinnedMeshComponent>(ref.skinnedMeshEntity))
                skinned->lastUpdateFrameIndex = frameIndex;
        });
}

void SceneEntityWorld::systemApplyDeferredCommands(ecs::World& world, const ecs::ScheduleContext& /*ctx*/)
{
    if (auto* commands = world.getResource<ecs::CommandQueue>())
    {
        if (!commands->empty())
            commands->apply(world);
    }
}

void SceneEntityWorld::markTransformDirty()
{
    m_transformDirty = true;
}

void SceneEntityWorld::markSkinnedMeshDirtyForJoint(ecs::Entity jointEntity)
{
    if (!ecs::isValid(jointEntity))
        return;

    const auto* ref = m_world.get<SkinnedMeshReferenceComponent>(jointEntity);
    if (!ref || !ecs::isValid(ref->skinnedMeshEntity))
        return;

    if (auto* skinned = m_world.get<SkinnedMeshComponent>(ref->skinnedMeshEntity))
        skinned->lastUpdateFrameIndex = kForceSkinnedMeshUpdateFrameIndex;
}

void SceneEntityWorld::systemRefreshInstanceIndices(ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/)
{
    if (!m_frameStructureDirty)
        return;

    refreshInstanceIndices();
}

void SceneEntityWorld::systemAssignGlobalResourceIndices(ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/)
{
    if (!m_frameStructureDirty)
        return;

    assignGlobalResourceIndices();
}

void SceneEntityWorld::systemFinalizeFrameFlags(ecs::World& world, const ecs::ScheduleContext& /*ctx*/)
{
    m_previousTransformDirty = m_frameStructureDirty || m_frameTransformDirty;
    m_structureDirty = false;
    m_transformDirty = false;
    world.endChangeFrame();
    if (auto* changeDetection = world.getResource<ecs::ChangeDetection>())
        changeDetection->clearWorldStructureChange();
    if (auto* transformEvents = world.getResource<ecs::Events<TransformChangedEvent>>())
        transformEvents->clear();
}

void SceneEntityWorld::refreshInstanceIndices()
{
    int instanceIndex = 0;
    int geometryInstanceIndex = 0;

    m_world.each<MeshInstanceComponent>([&](ecs::Entity, MeshInstanceComponent& mesh)
    {
        mesh.instanceIndex = instanceIndex++;
        mesh.geometryInstanceIndex = geometryInstanceIndex;
        if (mesh.mesh)
            geometryInstanceIndex += static_cast<int>(mesh.mesh->geometries.size());
    });

    m_GeometryInstancesCount = static_cast<size_t>(geometryInstanceIndex);
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
    m_world.each<AnimationComponent>([&](ecs::Entity, AnimationComponent& animation) {
        (void)ApplyAnimation(animation, time, *this);
    });
}

ecs::Entity SceneEntityWorld::createEntity(const std::string& name, ecs::Entity parent)
{
    ensureChangeDetection();
    ecs::Entity entity = m_world.spawn();
    m_world.emplace<NameComponent>(entity, NameComponent{ name });
    m_world.emplace<ChildrenComponent>(entity, ChildrenComponent{});
    m_world.emplace<LocalTransformComponent>(entity, LocalTransformComponent{});
    m_world.emplace<GlobalTransformComponent>(entity, GlobalTransformComponent{});
    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{});
    m_world.emplace<BoundsComponent>(entity, BoundsComponent{});
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{});

    if (!ecs::isValid(m_root))
    {
        m_root = entity;
        m_world.insertResource<SceneRootResource>(SceneRootResource{ m_root });
    }

    if (ecs::isValid(parent))
        setParent(entity, parent);

    return entity;
}

void SceneEntityWorld::unregisterEntityLeaves(ecs::Entity entity)
{
    if (m_world.has<CameraComponent>(entity))
        unregisterCameraEntity(entity);

    if (auto* mesh = m_world.get<MeshInstanceComponent>(entity))
        UnregisterMeshInstanceEntity(entity, mesh->mesh, m_world.has<SkinnedMeshComponent>(entity));
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
    m_world.notifyComponentChanged<LocalTransformComponent>(entity);
    m_world.events<TransformChangedEvent>().send(TransformChangedEvent{ entity });
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
        if (mesh->mesh)
        {
            leafContent = GetMeshContentFlags(*mesh->mesh);
            localBounds = GetMeshLocalBounds(*mesh->mesh);
        }
    }
    else if (m_world.has<CameraComponent>(entity))
        leafContent = GetCameraContentFlags();
    else if (m_world.has<LightComponent>(entity))
        leafContent = GetLightContentFlags();
    else if (m_world.has<AnimationComponent>(entity))
        leafContent = GetAnimationContentFlags();

    m_world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{ localBounds });
    m_world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leafContent,
        .subgraphContent = leafContent,
    });
}

void SceneEntityWorld::setMeshInstance(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    MeshInstanceComponent component;
    InitializeMeshInstanceComponent(component, mesh);
    m_world.emplace<MeshInstanceComponent>(entity, std::move(component));
    RegisterMeshInstanceEntity(entity, mesh, false);
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setSkinnedMeshInstance(
    ecs::Entity entity, SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh)
{
    auto skinnedMesh = CreateSkinnedMeshFromPrototype(factory, prototypeMesh);

    MeshInstanceComponent component;
    InitializeMeshInstanceComponent(component, skinnedMesh);
    m_world.emplace<MeshInstanceComponent>(entity, std::move(component));

    SkinnedMeshComponent skinned;
    skinned.prototypeMesh = prototypeMesh;
    m_world.emplace<SkinnedMeshComponent>(entity, std::move(skinned));
    m_world.emplace<SkinnedMeshGpuComponent>(entity, SkinnedMeshGpuComponent{});

    RegisterMeshInstanceEntity(entity, skinnedMesh, true);
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setSkinnedMeshReference(ecs::Entity entity, ecs::Entity skinnedMeshEntity)
{
    if (!ecs::isValid(skinnedMeshEntity))
        return;
    m_world.emplace<SkinnedMeshReferenceComponent>(entity, SkinnedMeshReferenceComponent{ skinnedMeshEntity });
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setLight(ecs::Entity entity, LightComponent component)
{
    m_world.emplace<LightComponent>(entity, std::move(component));
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setLight(ecs::Entity entity, const std::shared_ptr<Light>& light)
{
    if (!light)
        return;
    LightComponent component;
    InitializeLightComponent(component, light);
    setLight(entity, std::move(component));
}

void SceneEntityWorld::setCamera(ecs::Entity entity, CameraComponent component)
{
    m_world.emplace<CameraComponent>(entity, std::move(component));
    registerCameraEntity(entity);
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setCamera(ecs::Entity entity, const std::shared_ptr<SceneCamera>& camera)
{
    if (!camera)
        return;
    CameraComponent component;
    InitializeCameraComponent(component, camera);
    setCamera(entity, std::move(component));
}

void SceneEntityWorld::setAnimation(ecs::Entity entity, AnimationComponent component)
{
    m_world.emplace<AnimationComponent>(entity, std::move(component));
    updateLeafContentAndBounds(entity);
}

void SceneEntityWorld::setAnimation(ecs::Entity entity, const std::shared_ptr<SceneAnimation>& animation)
{
    if (!animation)
        return;
    AnimationComponent component;
    InitializeAnimationComponent(component, animation);
    setAnimation(entity, std::move(component));
}

void SceneEntityWorld::setGaussianSplat(ecs::Entity entity, const std::shared_ptr<GaussianSplat>& splat)
{
    if (!splat)
        return;
    splat->ownerEntity = entity;
    m_world.emplace<GaussianSplatComponent>(entity, GaussianSplatComponent{ splat });
    updateLeafContentAndBounds(entity);
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

ecs::Entity SceneEntityWorld::importSubtree(
    ecs::Entity parent,
    const SceneEntityWorld& source,
    ecs::Entity sourceRoot,
    SceneTypeFactory* factory)
{
    if (!ecs::isValid(sourceRoot))
        return ecs::NullEntity;

    std::unordered_map<ecs::Entity, ecs::Entity> entityMap;
    std::vector<ecs::Entity> importedSkinnedEntities;
    std::vector<ecs::Entity> importedAnimationEntities;
    ecs::Entity newRoot = ecs::NullEntity;

    std::function<void(ecs::Entity, ecs::Entity)> copyRecursive =
        [&](ecs::Entity srcEntity, ecs::Entity dstParent) {
            const auto* srcName = source.m_world.get<NameComponent>(srcEntity);
            const std::string name = srcName ? srcName->value : std::string{};
            ecs::Entity dstEntity = createEntity(name, dstParent);
            entityMap[srcEntity] = dstEntity;

            if (srcEntity == sourceRoot)
                newRoot = dstEntity;

            CopyEntityComponents(m_world, dstEntity, source.m_world, srcEntity, false);

            if (m_world.has<CameraComponent>(dstEntity))
                registerCameraEntity(dstEntity);

            if (m_world.has<AnimationComponent>(dstEntity))
                importedAnimationEntities.push_back(dstEntity);

            const auto* srcMesh = source.m_world.get<MeshInstanceComponent>(srcEntity);
            const auto* srcSkinned = source.m_world.get<SkinnedMeshComponent>(srcEntity);
            if (srcMesh && srcMesh->mesh)
            {
                if (srcSkinned && srcSkinned->prototypeMesh && factory)
                {
                    setSkinnedMeshInstance(dstEntity, *factory, srcSkinned->prototypeMesh);

                    if (auto* dstMesh = m_world.get<MeshInstanceComponent>(dstEntity))
                    {
                        std::shared_ptr<MeshInfo> skinnedMesh = dstMesh->mesh;
                        *dstMesh = *srcMesh;
                        dstMesh->mesh = std::move(skinnedMesh);
                    }

                    if (auto* dstSkinned = m_world.get<SkinnedMeshComponent>(dstEntity))
                    {
                        *dstSkinned = *srcSkinned;
                        dstSkinned->lastUpdateFrameIndex = 0;
                    }
                    importedSkinnedEntities.push_back(dstEntity);
                }
                else
                {
                    m_world.emplace<MeshInstanceComponent>(dstEntity, *srcMesh);
                    if (srcSkinned)
                    {
                        SkinnedMeshComponent copiedSkinned = *srcSkinned;
                        copiedSkinned.lastUpdateFrameIndex = 0;
                        m_world.emplace<SkinnedMeshComponent>(dstEntity, std::move(copiedSkinned));
                        m_world.emplace<SkinnedMeshGpuComponent>(dstEntity, SkinnedMeshGpuComponent{});
                        importedSkinnedEntities.push_back(dstEntity);
                    }
                    RegisterMeshInstanceEntity(dstEntity, srcMesh->mesh, srcSkinned != nullptr);
                }
            }

            if (const auto* children = source.m_world.get<ChildrenComponent>(srcEntity))
            {
                for (ecs::Entity srcChild : children->children)
                    copyRecursive(srcChild, dstEntity);
            }
        };

    copyRecursive(sourceRoot, parent);

    // Only remap joints on skinned meshes created by this import. Remapping all
    // SkinnedMeshComponents causes entity-id collisions when later models are imported.
    for (ecs::Entity skinnedEntity : importedSkinnedEntities)
    {
        auto* skinned = m_world.get<SkinnedMeshComponent>(skinnedEntity);
        if (!skinned)
            continue;

        for (SkinnedMeshJoint& joint : skinned->joints)
        {
            if (!ecs::isValid(joint.jointEntity))
                continue;
            auto it = entityMap.find(joint.jointEntity);
            if (it != entityMap.end())
                joint.jointEntity = it->second;
        }
    }

    for (const auto& [srcEntity, dstEntity] : entityMap)
    {
        (void)srcEntity;
        auto* ref = m_world.get<SkinnedMeshReferenceComponent>(dstEntity);
        if (!ref || !ecs::isValid(ref->skinnedMeshEntity))
            continue;
        auto it = entityMap.find(ref->skinnedMeshEntity);
        if (it != entityMap.end())
            ref->skinnedMeshEntity = it->second;
    }

    m_world.each<MeshInstanceComponent>([&](ecs::Entity entity, MeshInstanceComponent& mesh) {
        if (!ecs::isValid(mesh.proxiedAnalyticLight))
            return;
        auto it = entityMap.find(mesh.proxiedAnalyticLight);
        if (it != entityMap.end())
            mesh.proxiedAnalyticLight = it->second;
    });

    // Only remap animation channels on animations created by this import. Remapping
    // every AnimationComponent causes entity-id collisions when later models load.
    for (ecs::Entity animEntity : importedAnimationEntities)
    {
        auto* animation = m_world.get<AnimationComponent>(animEntity);
        if (!animation)
            continue;

        for (AnimationChannelData& channel : animation->channels)
        {
            if (!ecs::isValid(channel.targetEntity))
                continue;

            auto it = entityMap.find(channel.targetEntity);
            if (it != entityMap.end())
                channel.targetEntity = it->second;
        }
    }

    rebuildPathsFromRoot();
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
