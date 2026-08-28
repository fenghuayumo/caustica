#include <scene/SceneEcs.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneLightAccess.h>

#include <ecs/ChangeDetection.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <cstring>

namespace caustica::scene
{

SceneContentFlags getMeshContentFlags(const MeshInfo& mesh)
{
    SceneContentFlags flags = SceneContentFlags::None;

    for (const auto& geometry : mesh.geometries)
    {
        if (!geometry->material)
            continue;

        switch (geometry->material->domain) // NOLINT(clang-diagnostic-switch-enum)
        {
        case MaterialDomain::Opaque:
            flags |= SceneContentFlags::OpaqueMeshes;
            break;
        case MaterialDomain::AlphaTested:
            flags |= SceneContentFlags::AlphaTestedMeshes;
            break;
        default:
            flags |= SceneContentFlags::BlendedMeshes;
            break;
        }
    }

    return flags;
}

dm::box3 getMeshLocalBounds(const MeshInfo& mesh)
{
    return mesh.objectSpaceBounds;
}

bool setMeshProperty(MeshInfo& mesh, const std::string& propName, const dm::float4& value)
{
    if (mesh.geometries.size() == 1 && mesh.geometries[0]->material)
        return mesh.geometries[0]->material->setProperty(propName, value);

    return false;
}

void initializeMeshInstanceComponent(MeshInstanceComponent& component, const std::shared_ptr<MeshInfo>& mesh)
{
    component.mesh = mesh;
    component.instanceIndex = -1;
    component.geometryInstanceIndex = -1;
    component.proxiedAnalyticLight = ecs::NullEntity;
    component.enabled = true;
}

std::shared_ptr<MeshInfo> createSkinnedMeshFromPrototype(
    SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh)
{
    auto skinnedMesh = factory.createMesh();
    skinnedMesh->skinPrototype = prototypeMesh;
    skinnedMesh->name = prototypeMesh->name;
    skinnedMesh->objectSpaceBounds = prototypeMesh->objectSpaceBounds;
    skinnedMesh->indexOffset = prototypeMesh->indexOffset;
    skinnedMesh->vertexOffset = 0;
    skinnedMesh->totalVertices = prototypeMesh->totalVertices;
    skinnedMesh->totalIndices = prototypeMesh->totalIndices;
    skinnedMesh->geometries.reserve(prototypeMesh->geometries.size());

    for (const auto& geometry : prototypeMesh->geometries)
    {
        auto newGeometry = factory.createMeshGeometry();
        *newGeometry = *geometry;
        skinnedMesh->geometries.push_back(std::move(newGeometry));
    }

    return skinnedMesh;
}

namespace
{
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

    local->compose();

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

    const dm::affine3 previousFloat = global->transformFloat;
    global->transformFloat = dm::affine3(global->transform);
    // Extract patches mesh proxies via Changed<GlobalTransformComponent>. Mutating
    // fields in-place does not mark the component; notify when the global pose moves.
    if (std::memcmp(&previousFloat, &global->transformFloat, sizeof(previousFloat)) != 0)
        world.notifyComponentChanged<GlobalTransformComponent>(entity);

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
    }
    if (const auto* skinRef = srcWorld.get<SkinnedMeshReferenceComponent>(srcEntity))
        dstWorld.emplace<SkinnedMeshReferenceComponent>(dstEntity, *skinRef);
    if (const auto* prefab = srcWorld.get<PrefabInstanceComponent>(srcEntity))
        dstWorld.emplace<PrefabInstanceComponent>(dstEntity, *prefab);
    if (const auto* materialOverride = srcWorld.get<MaterialOverrideComponent>(srcEntity))
        dstWorld.emplace<MaterialOverrideComponent>(dstEntity, *materialOverride);
    if (const auto* directional = srcWorld.get<DirectionalLightComponent>(srcEntity))
        dstWorld.emplace<DirectionalLightComponent>(dstEntity, *directional);
    if (const auto* spot = srcWorld.get<SpotLightComponent>(srcEntity))
        dstWorld.emplace<SpotLightComponent>(dstEntity, *spot);
    if (const auto* point = srcWorld.get<PointLightComponent>(srcEntity))
        dstWorld.emplace<PointLightComponent>(dstEntity, *point);
    if (const auto* environment = srcWorld.get<EnvironmentLightComponent>(srcEntity))
        dstWorld.emplace<EnvironmentLightComponent>(dstEntity, *environment);
    if (const auto* camera = srcWorld.get<CameraComponent>(srcEntity))
        dstWorld.emplace<CameraComponent>(dstEntity, *camera);
    if (const auto* animation = srcWorld.get<AnimationComponent>(srcEntity))
        dstWorld.emplace<AnimationComponent>(dstEntity, *animation);
    if (const auto* geomSeq = srcWorld.get<GeometrySequenceComponent>(srcEntity))
        dstWorld.emplace<GeometrySequenceComponent>(dstEntity, *geomSeq);
    if (const auto* splat = srcWorld.get<GaussianSplatComponent>(srcEntity))
        dstWorld.emplace<GaussianSplatComponent>(dstEntity, *splat);
    if (const auto* sceneSettings = srcWorld.get<SceneSettingsComponent>(srcEntity))
        dstWorld.emplace<SceneSettingsComponent>(dstEntity, *sceneSettings);
    if (const auto* game = srcWorld.get<GameSettingsComponent>(srcEntity))
        dstWorld.emplace<GameSettingsComponent>(dstEntity, *game);
}
} // namespace

void updateHierarchy(ecs::World& world, PreviousTransformPolicy previousPolicy)
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

SceneEntityWorld::SceneEntityWorld()
    : m_owned(std::make_unique<ecs::World>())
    , m_world(m_owned.get())
{
}

SceneEntityWorld::SceneEntityWorld(ecs::World& liveWorld)
    : m_world(&liveWorld)
{
}

SceneEntityWorld::~SceneEntityWorld()
{
    if (!m_owned)
        resetScene();
}

void SceneEntityWorld::resetScene()
{
    if (!m_world)
        return;

    if (ecs::isValid(m_root) && m_world->isAlive(m_root))
        destroyEntity(m_root);

    m_root = ecs::NullEntity;
    m_CameraEntities.clear();
    m_pathToEntity.clear();
    m_registeredMeshByEntity.clear();
    m_Materials = {};
    m_Meshes = {};
    m_GeometryCount = 0;
    m_MaxGeometryCountPerMesh = 0;
    m_GeometryInstancesCount = 0;
    m_structureDirty = true;
    m_transformDirty = true;
    m_lightDirty = true;
    m_frameStructureDirty = false;
    m_frameTransformDirty = false;
    m_frameLightDirty = false;
    m_previousTransformDirty = false;
}

void SceneEntityWorld::adoptInto(ecs::World& liveWorld, SceneTypeFactory* factory)
{
    if (m_world == &liveWorld)
        return;

    const ecs::Entity sourceRoot = m_root;
    SceneEntityWorld dest(liveWorld);
    if (ecs::isValid(sourceRoot) && m_world && m_world->isAlive(sourceRoot))
        dest.importSubtree(ecs::NullEntity, *this, sourceRoot, factory);

    m_root = dest.m_root;
    m_CameraEntities = std::move(dest.m_CameraEntities);
    m_pathToEntity = std::move(dest.m_pathToEntity);
    m_registeredMeshByEntity = std::move(dest.m_registeredMeshByEntity);
    m_Materials = std::move(dest.m_Materials);
    m_Meshes = std::move(dest.m_Meshes);
    m_GeometryCount = dest.m_GeometryCount;
    m_MaxGeometryCountPerMesh = dest.m_MaxGeometryCountPerMesh;
    m_GeometryInstancesCount = dest.m_GeometryInstancesCount;
    dest.m_GeometryCount = 0;
    dest.m_MaxGeometryCountPerMesh = 0;
    dest.m_GeometryInstancesCount = 0;
    dest.m_root = ecs::NullEntity;
    dest.m_world = nullptr;

    m_world = &liveWorld;
    m_owned.reset();

    m_structureDirty = true;
    m_transformDirty = true;
    m_lightDirty = true;
    ensureChangeDetection();
}

void SceneEntityWorld::registerCameraEntity(ecs::Entity entity)
{
    if (!m_world->isAlive(entity) || !m_world->has<CameraComponent>(entity))
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
    if (m_owned)
        m_world->clear();
    else
        resetScene();

    if (m_owned)
    {
        m_root = ecs::NullEntity;
        m_CameraEntities.clear();
        m_pathToEntity.clear();
        m_registeredMeshByEntity.clear();
        m_Materials = {};
        m_Meshes = {};
        m_GeometryCount = 0;
        m_MaxGeometryCountPerMesh = 0;
        m_GeometryInstancesCount = 0;
        m_structureDirty = true;
        m_transformDirty = true;
        m_lightDirty = true;
        m_frameLightDirty = false;
        m_previousTransformDirty = false;
        ensureChangeDetection();
    }
}

void SceneEntityWorld::ensureChangeDetection()
{
    m_world->enableChangeDetection();
}

void SceneEntityWorld::syncDirtyFlagsFromChangeDetection()
{
    ensureChangeDetection();
    const auto* changeDetection = m_world->getResource<ecs::ChangeDetection>();
    if (!changeDetection)
        return;

    const auto& registry = m_world->registry();

    const bool worldStructureChanged = changeDetection->worldStructureChanged();
    const bool renderStructureChanged = worldStructureChanged
        || changeDetection->anyOfChangedThisFrame<
            MeshInstanceComponent,
            SkinnedMeshComponent,
            CameraComponent,
            AnimationComponent,
            GaussianSplatComponent,
            ParentComponent,
            ChildrenComponent>(registry)
        || changeDetection->anyOfAddedThisFrame<
            MeshInstanceComponent,
            SkinnedMeshComponent,
            CameraComponent,
            AnimationComponent,
            GaussianSplatComponent,
            ParentComponent,
            ChildrenComponent>(registry);

    if (renderStructureChanged)
    {
        m_structureDirty = true;
        m_transformDirty = true;
    }

    const bool lightComponentsChanged =
        changeDetection->anyOfChangedThisFrame<
            DirectionalLightComponent,
            SpotLightComponent,
            PointLightComponent,
            EnvironmentLightComponent>(registry)
        || changeDetection->anyOfAddedThisFrame<
            DirectionalLightComponent,
            SpotLightComponent,
            PointLightComponent,
            EnvironmentLightComponent>(registry);
    if (worldStructureChanged || lightComponentsChanged)
    {
        m_lightDirty = true;
    }

    // A transform on a light-only subtree must not masquerade as a mesh transform
    // change. The old global flag rewrote every mesh instance buffer while dragging
    // a light, disturbing temporal rendering even though no geometry moved.
    m_world->each<LocalTransformComponent, SceneContentComponent, ecs::Changed<LocalTransformComponent>>(
        [&](ecs::Entity, LocalTransformComponent&, SceneContentComponent& content) {
            const SceneContentFlags subtree = content.subgraphContent;
            const bool containsLights = (subtree & SceneContentFlags::Lights) != 0;
            const bool containsAnythingElse = (subtree & ~SceneContentFlags::Lights) != 0;
            if (containsLights)
                m_lightDirty = true;
            if (!containsLights || containsAnythingElse)
                m_transformDirty = true;
        });

    if (changeDetection->anyOfChangedThisFrame<ParentComponent>(registry)
        || changeDetection->anyOfAddedThisFrame<ParentComponent>(registry))
    {
        m_transformDirty = true;
        m_lightDirty = true;
    }
}

void SceneEntityWorld::ensureSceneResourcesSynced()
{
    syncSceneResourcesFromEcs();
}

const std::vector<ecs::Entity>& SceneEntityWorld::cameraEntitiesInRegistrationOrder() const
{
    const_cast<SceneEntityWorld*>(this)->syncSceneResourcesFromEcs();
    return m_CameraEntities;
}

void SceneEntityWorld::syncSceneResourcesFromEcs()
{
    ensureChangeDetection();
    const auto* changeDetection = m_world->getResource<ecs::ChangeDetection>();
    if (!changeDetection)
        return;

    const auto& registry = m_world->registry();

    const bool meshDirty =
        changeDetection->anyOfAddedThisFrame<MeshInstanceComponent, SkinnedMeshComponent>(registry)
        || changeDetection->anyOfChangedThisFrame<MeshInstanceComponent, SkinnedMeshComponent>(registry);

    const bool cameraDirty =
        changeDetection->anyOfAddedThisFrame<CameraComponent>(registry)
        || changeDetection->anyOfChangedThisFrame<CameraComponent>(registry);

    const bool leafDirty =
        meshDirty
        || cameraDirty
        || changeDetection->anyOfAddedThisFrame<
            DirectionalLightComponent,
            SpotLightComponent,
            PointLightComponent,
            EnvironmentLightComponent,
            AnimationComponent,
            GaussianSplatComponent,
            SkinnedMeshReferenceComponent>(registry)
        || changeDetection->anyOfChangedThisFrame<
            DirectionalLightComponent,
            SpotLightComponent,
            PointLightComponent,
            EnvironmentLightComponent,
            AnimationComponent,
            GaussianSplatComponent,
            SkinnedMeshReferenceComponent>(registry);

    if (meshDirty)
    {
        m_world->each<MeshInstanceComponent>(
            [&](ecs::Entity entity, MeshInstanceComponent& meshInstance) {
                const bool added = changeDetection->isAddedThisFrame<MeshInstanceComponent>(entity, registry);
                const bool changed = changeDetection->isChangedThisFrame<MeshInstanceComponent>(entity, registry);
                if (!added && !changed)
                    return;

                const std::shared_ptr<MeshInfo>& newMesh = meshInstance.mesh;
                const bool skinned = m_world->has<SkinnedMeshComponent>(entity);
                auto it = m_registeredMeshByEntity.find(entity);
                if (it != m_registeredMeshByEntity.end())
                {
                    if (it->second == newMesh)
                        return;
                    unregisterMeshInstanceEntity(entity, it->second, skinned);
                    if (!newMesh)
                    {
                        m_registeredMeshByEntity.erase(it);
                        return;
                    }
                    it->second = newMesh;
                }
                else if (!newMesh)
                {
                    return;
                }
                else
                {
                    m_registeredMeshByEntity.emplace(entity, newMesh);
                }

                registerMeshInstanceEntity(entity, newMesh, skinned);
            });

        // Component removed while entity lives (rare): drop stale registrations.
        for (auto it = m_registeredMeshByEntity.begin(); it != m_registeredMeshByEntity.end();)
        {
            if (!m_world->isAlive(it->first) || !m_world->has<MeshInstanceComponent>(it->first))
            {
                if (m_world->isAlive(it->first))
                    unregisterMeshInstanceEntity(
                        it->first, it->second, m_world->has<SkinnedMeshComponent>(it->first));
                it = m_registeredMeshByEntity.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (cameraDirty)
    {
        m_world->each<CameraComponent>([&](ecs::Entity entity, CameraComponent&) {
            if (changeDetection->isAddedThisFrame<CameraComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<CameraComponent>(entity, registry))
            {
                registerCameraEntity(entity);
            }
        });

        m_CameraEntities.erase(
            std::remove_if(
                m_CameraEntities.begin(),
                m_CameraEntities.end(),
                [this](ecs::Entity entity) {
                    return !m_world->isAlive(entity) || !m_world->has<CameraComponent>(entity);
                }),
            m_CameraEntities.end());
    }

    if (leafDirty)
    {
        auto updateIfTouched = [&](ecs::Entity entity) {
            updateLeafContentAndBounds(entity);
        };

        m_world->each<MeshInstanceComponent>([&](ecs::Entity entity, MeshInstanceComponent&) {
            if (changeDetection->isAddedThisFrame<MeshInstanceComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<MeshInstanceComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<CameraComponent>([&](ecs::Entity entity, CameraComponent&) {
            if (changeDetection->isAddedThisFrame<CameraComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<CameraComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<DirectionalLightComponent>([&](ecs::Entity entity, DirectionalLightComponent&) {
            if (changeDetection->isAddedThisFrame<DirectionalLightComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<DirectionalLightComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<SpotLightComponent>([&](ecs::Entity entity, SpotLightComponent&) {
            if (changeDetection->isAddedThisFrame<SpotLightComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<SpotLightComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<PointLightComponent>([&](ecs::Entity entity, PointLightComponent&) {
            if (changeDetection->isAddedThisFrame<PointLightComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<PointLightComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<EnvironmentLightComponent>([&](ecs::Entity entity, EnvironmentLightComponent&) {
            if (changeDetection->isAddedThisFrame<EnvironmentLightComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<EnvironmentLightComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<AnimationComponent>([&](ecs::Entity entity, AnimationComponent&) {
            if (changeDetection->isAddedThisFrame<AnimationComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<AnimationComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<GaussianSplatComponent>([&](ecs::Entity entity, GaussianSplatComponent&) {
            if (changeDetection->isAddedThisFrame<GaussianSplatComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<GaussianSplatComponent>(entity, registry))
                updateIfTouched(entity);
        });
        m_world->each<SkinnedMeshReferenceComponent>([&](ecs::Entity entity, SkinnedMeshReferenceComponent&) {
            if (changeDetection->isAddedThisFrame<SkinnedMeshReferenceComponent>(entity, registry)
                || changeDetection->isChangedThisFrame<SkinnedMeshReferenceComponent>(entity, registry))
                updateIfTouched(entity);
        });
    }
}

bool SceneEntityWorld::hasPendingStructureChanges()
{
    syncDirtyFlagsFromChangeDetection();
    return m_structureDirty;
}

bool SceneEntityWorld::hasPendingTransformChanges()
{
    syncDirtyFlagsFromChangeDetection();
    return m_transformDirty || m_previousTransformDirty;
}

bool SceneEntityWorld::hasPendingLightChanges()
{
    syncDirtyFlagsFromChangeDetection();
    // Component edits remain visible through Changed<T> until Extract, but a
    // deleted light has no component left to carry that signal. Preserve the
    // refresh-time snapshot until publishRenderSnapshot ends the ECS frame.
    return m_lightDirty || m_frameLightDirty;
}

void SceneEntityWorld::refreshHierarchy(PreviousTransformPolicy previousPolicy)
{
    updateHierarchy(*m_world, previousPolicy);
}

void SceneEntityWorld::syncPreviousTransformsFromCurrent()
{
    m_world->each<GlobalTransformComponent>([](ecs::Entity, GlobalTransformComponent& global) {
        global.previousTransform = global.transform;
        global.previousTransformFloat = global.transformFloat;
    });
    m_previousTransformDirty = true;
}

void SceneEntityWorld::refresh(uint32_t frameIndex)
{
    beginRefreshFrame();
    refreshHierarchy(PreviousTransformPolicy::CaptureCurrent);
    markDirtySkinnedMeshesFromChangedJoints(frameIndex);
    markDirtySkinnedMeshes(frameIndex);
    refreshInstanceIndicesIfNeeded();
    assignGlobalResourceIndicesIfNeeded();
    applyDeferredCommands();
    finalizeRefreshFrame();
}

void SceneEntityWorld::beginRefreshFrame()
{
    ensureChangeDetection();
    // Emplace-only writes land here: register meshes/cameras + leaf bounds
    // before dirty hydration and hierarchy/instance index work.
    syncSceneResourcesFromEcs();
    syncDirtyFlagsFromChangeDetection();

    m_frameStructureDirty = m_structureDirty;
    m_frameTransformDirty = m_transformDirty;
    m_frameLightDirty |= m_lightDirty;
}

void SceneEntityWorld::markDirtySkinnedMeshes(uint32_t frameIndex)
{
    if (!(m_frameTransformDirty || m_frameStructureDirty))
        return;

    m_world->each<SkinnedMeshReferenceComponent>(
        [frameIndex, this](ecs::Entity, SkinnedMeshReferenceComponent& ref) {
            if (!ecs::isValid(ref.skinnedMeshEntity))
                return;
            if (auto* skinned = m_world->get<SkinnedMeshComponent>(ref.skinnedMeshEntity))
                skinned->lastUpdateFrameIndex = frameIndex;
        });
}

void SceneEntityWorld::markDirtySkinnedMeshesFromChangedJoints(uint32_t frameIndex)
{
    m_world->each<SkinnedMeshReferenceComponent, ecs::Changed<LocalTransformComponent>>(
        [&](ecs::Entity, SkinnedMeshReferenceComponent& ref, LocalTransformComponent&) {
            if (!ecs::isValid(ref.skinnedMeshEntity))
                return;
            if (auto* skinned = m_world->get<SkinnedMeshComponent>(ref.skinnedMeshEntity))
                skinned->lastUpdateFrameIndex = frameIndex;
        });
}

void SceneEntityWorld::resetSkinnedMeshMotionHistory()
{
    m_world->each<SkinnedMeshComponent>([](ecs::Entity, SkinnedMeshComponent& skinned) {
        skinned.resetMotionHistory = true;
        skinned.lastUpdateFrameIndex = kForceSkinnedMeshUpdateFrameIndex;
    });
}

void SceneEntityWorld::applyDeferredCommands()
{
    // Live App world: App::runSchedule is the sole CommandQueue apply.
    if (!m_owned)
        return;
    if (auto* commands = m_world->getResource<ecs::CommandQueue>())
    {
        if (!commands->empty())
            commands->apply(*m_world);
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

    const auto* ref = m_world->get<SkinnedMeshReferenceComponent>(jointEntity);
    if (!ref || !ecs::isValid(ref->skinnedMeshEntity))
        return;

    if (auto* skinned = m_world->get<SkinnedMeshComponent>(ref->skinnedMeshEntity))
        skinned->lastUpdateFrameIndex = kForceSkinnedMeshUpdateFrameIndex;
}

void SceneEntityWorld::refreshInstanceIndicesIfNeeded()
{
    if (!m_frameStructureDirty)
        return;

    refreshInstanceIndices();
}

void SceneEntityWorld::assignGlobalResourceIndicesIfNeeded()
{
    if (!m_frameStructureDirty)
        return;

    assignGlobalResourceIndices();
}

void SceneEntityWorld::finalizeRefreshFrame()
{
    // Keep ChangeDetection tick open for Extract / other Changed<> readers.
    // Sticky scene dirty bits can clear; the ECS change tick ends in
    // endChangeDetectionFrame() after extractAndPublishRenderSnapshot.
    m_previousTransformDirty = m_frameStructureDirty || m_frameTransformDirty;
    m_structureDirty = false;
    m_transformDirty = false;
    m_lightDirty = false;
}

void SceneEntityWorld::endChangeDetectionFrame()
{
    m_world->endChangeFrame();
    m_frameLightDirty = false;
    if (auto* changeDetection = m_world->getResource<ecs::ChangeDetection>())
        changeDetection->clearWorldStructureChange();
    if (auto* transformEvents = m_world->getResource<ecs::Events<TransformChangedEvent>>())
        transformEvents->clear();
}

void SceneEntityWorld::refreshInstanceIndices()
{
    int instanceIndex = 0;
    int geometryInstanceIndex = 0;

    // Collect then sort by entity id so GaussianSplat (and other non-mesh) archetypes
    // cannot reshuffle EnTT iteration order across imports / deletes.
    struct MeshInstanceRef
    {
        ecs::Entity entity = ecs::NullEntity;
        MeshInstanceComponent* mesh = nullptr;
    };
    std::vector<MeshInstanceRef> instances;
    m_world->each<MeshInstanceComponent, GlobalTransformComponent, BoundsComponent, SceneContentComponent>(
        [&](ecs::Entity entity, MeshInstanceComponent& mesh, GlobalTransformComponent&, BoundsComponent&, SceneContentComponent&)
        {
            instances.push_back(MeshInstanceRef{ entity, &mesh });
        });
    std::sort(instances.begin(), instances.end(), [](const MeshInstanceRef& a, const MeshInstanceRef& b) {
        return static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity);
    });

    for (MeshInstanceRef& entry : instances)
    {
        entry.mesh->instanceIndex = instanceIndex++;
        entry.mesh->geometryInstanceIndex = geometryInstanceIndex;
        if (entry.mesh->mesh)
            geometryInstanceIndex += static_cast<int>(entry.mesh->mesh->geometries.size());
    }

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
    m_world->each<AnimationComponent>([&](ecs::Entity, AnimationComponent& animation) {
        (void)applyAnimation(animation, time, *this);
    });
}

ecs::Entity SceneEntityWorld::createEntity(const std::string& name, ecs::Entity parent)
{
    ensureChangeDetection();
    ecs::Entity entity = m_world->spawn();
    m_world->emplace<NameComponent>(entity, NameComponent{ name });
    m_world->emplace<ChildrenComponent>(entity, ChildrenComponent{});
    m_world->emplace<LocalTransformComponent>(entity, LocalTransformComponent{});
    m_world->emplace<GlobalTransformComponent>(entity, GlobalTransformComponent{});
    m_world->emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{});
    m_world->emplace<BoundsComponent>(entity, BoundsComponent{});
    m_world->emplace<SceneContentComponent>(entity, SceneContentComponent{});

    if (!ecs::isValid(m_root))
    {
        m_root = entity;
        m_world->insertResource<SceneRootResource>(SceneRootResource{ m_root });
    }

    if (ecs::isValid(parent))
        setParent(entity, parent);
    else if (ecs::isValid(m_root) && entity != m_root)
        setParent(entity, m_root);

    return entity;
}

void SceneEntityWorld::unregisterEntityLeaves(ecs::Entity entity)
{
    if (m_world->has<CameraComponent>(entity))
        unregisterCameraEntity(entity);

    if (auto* mesh = m_world->get<MeshInstanceComponent>(entity))
        unregisterMeshInstanceEntity(entity, mesh->mesh, m_world->has<SkinnedMeshComponent>(entity));

    m_registeredMeshByEntity.erase(entity);
}

void SceneEntityWorld::destroyEntity(ecs::Entity entity)
{
    if (!m_world->isAlive(entity))
        return;

    const auto* content = m_world->get<SceneContentComponent>(entity);
    const SceneContentFlags subtree = content ? content->subgraphContent : SceneContentFlags::None;
    const bool containsLights = (subtree & SceneContentFlags::Lights) != 0;
    const bool lightOnly = containsLights && (subtree & ~SceneContentFlags::Lights) == 0;

    if (lightOnly)
    {
        // Analytic/environment lights are render proxies, not GPU scene structure.
        // Rebuilding mesh uploads, BLAS/TLAS and SBT for their removal is both
        // unnecessary and can overlap the next presented frame.
        m_lightDirty = true;
    }
    else
    {
        m_structureDirty = true;
        m_transformDirty = true;
        if (containsLights)
            m_lightDirty = true;
    }

    if (auto* children = m_world->get<ChildrenComponent>(entity))
    {
        auto childCopy = children->children;
        for (ecs::Entity child : childCopy)
            destroyEntity(child);
    }

    unregisterEntityLeaves(entity);

    if (auto* parent = m_world->get<ParentComponent>(entity))
        RemoveChildReference(*m_world, parent->parent, entity);

    if (auto* path = m_world->get<PathComponent>(entity))
        m_pathToEntity.erase(path->value.generic_string());

    if (entity == m_root)
    {
        m_root = ecs::NullEntity;
        if (auto* root = m_world->getResource<SceneRootResource>())
            root->root = ecs::NullEntity;
    }

    // ChangeDetection's generic structure bit implies a full mesh/AS rebuild.
    // Light-only deletion is tracked by m_lightDirty instead.
    m_world->despawn(entity, !lightOnly);
}

bool SceneEntityWorld::setParent(ecs::Entity entity, ecs::Entity parent)
{
    if (!m_world->isAlive(entity))
        return false;
    if (ecs::isValid(parent) && (!m_world->isAlive(parent) || parent == entity || IsDescendantOf(*m_world, parent, entity)))
        return false;

    ecs::Entity oldParent = ecs::NullEntity;
    if (auto* oldParentComponent = m_world->get<ParentComponent>(entity))
        oldParent = oldParentComponent->parent;

    if (oldParent == parent)
        return true;

    RemoveChildReference(*m_world, oldParent, entity);

    if (ecs::isValid(parent))
    {
        m_world->emplace<ParentComponent>(entity, ParentComponent{ parent });
        auto* children = m_world->get<ChildrenComponent>(parent);
        if (!children)
            children = &m_world->emplace<ChildrenComponent>(parent, ChildrenComponent{});
        if (std::find(children->children.begin(), children->children.end(), entity) == children->children.end())
        {
            children->children.push_back(entity);
            m_world->notifyComponentChanged<ChildrenComponent>(parent);
        }
    }
    else
    {
        m_world->remove<ParentComponent>(entity);
    }

    return true;
}

void SceneEntityWorld::setLocalTransform(
    ecs::Entity entity,
    const dm::double3* translation,
    const dm::dquat* rotation,
    const dm::double3* scaling)
{
    if (!m_world->isAlive(entity))
        return;

    auto* local = m_world->get<LocalTransformComponent>(entity);
    if (!local)
        local = &m_world->emplace<LocalTransformComponent>(entity, LocalTransformComponent{});

    bool changed = !local->hasLocalTransform;
    if (translation && any(*translation != local->translation))
    {
        local->translation = *translation;
        changed = true;
    }
    if (rotation)
    {
        // q and -q are the same orientation; avoid thrashing on sign flips.
        const double align = dm::dot(local->rotation, *rotation);
        const dm::dquat canonical = (align < 0.0) ? -(*rotation) : *rotation;
        if (any(canonical != local->rotation))
        {
            local->rotation = canonical;
            changed = true;
        }
    }
    if (scaling && any(*scaling != local->scaling))
    {
        local->scaling = *scaling;
        changed = true;
    }

    if (!changed)
        return;

    local->hasLocalTransform = true;
    local->compose();
    m_world->notifyComponentChanged<LocalTransformComponent>(entity);
    m_world->events<TransformChangedEvent>().send(TransformChangedEvent{ entity });
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
    if (!m_world->isAlive(entity))
        return;

    if (auto* current = m_world->get<PathComponent>(entity))
        m_pathToEntity.erase(current->value.generic_string());

    m_world->emplace<PathComponent>(entity, PathComponent{ path });
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

    if (auto* mesh = m_world->get<MeshInstanceComponent>(entity))
    {
        if (mesh->mesh)
        {
            leafContent = getMeshContentFlags(*mesh->mesh);
            localBounds = getMeshLocalBounds(*mesh->mesh);
        }
    }
    else if (m_world->has<GaussianSplatComponent>(entity))
    {
        // Local AABB is filled after GaussianSplatPass load; preserve it across refresh.
        if (const auto* existing = m_world->get<LocalBoundsComponent>(entity))
            localBounds = existing->bounds;
    }
    else if (m_world->has<CameraComponent>(entity))
        leafContent = getCameraContentFlags();
    else if (hasAnyLightComponent(*m_world, entity))
        leafContent = getLightContentFlags();
    else if (m_world->has<AnimationComponent>(entity))
        leafContent = getAnimationContentFlags();

    m_world->emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{ localBounds });
    m_world->emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leafContent,
        .subgraphContent = leafContent,
    });
}

void SceneEntityWorld::setMeshInstance(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    MeshInstanceComponent component;
    initializeMeshInstanceComponent(component, mesh);
    m_world->emplace<MeshInstanceComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, NameComponent component)
{
    m_world->emplace<NameComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, LocalTransformComponent component)
{
    if (!component.hasLocalTransform)
        return;
    // Compose local matrix + Changed<> notify (not scene resource sync).
    setLocalTransform(entity, &component.translation, &component.rotation, &component.scaling);
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, MeshInstanceComponent component)
{
    if (component.mesh)
    {
        // Preserve authoring flags; reset dense GPU indices like setMeshInstance.
        const bool enabled = component.enabled;
        const ecs::Entity proxied = component.proxiedAnalyticLight;
        initializeMeshInstanceComponent(component, component.mesh);
        component.enabled = enabled;
        component.proxiedAnalyticLight = proxied;
    }
    m_world->emplace<MeshInstanceComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, DirectionalLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<DirectionalLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, SpotLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<SpotLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, PointLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<PointLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, EnvironmentLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<EnvironmentLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, CameraComponent component)
{
    m_world->emplace<CameraComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, AnimationComponent component)
{
    m_world->emplace<AnimationComponent>(entity, std::move(component));
}

void SceneEntityWorld::insertSpawnComponent(ecs::Entity entity, GaussianSplatComponent component)
{
    m_world->emplace<GaussianSplatComponent>(entity, std::move(component));
}

void SceneEntityWorld::setSkinnedMeshInstance(
    ecs::Entity entity, SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh)
{
    auto skinnedMesh = createSkinnedMeshFromPrototype(factory, prototypeMesh);

    MeshInstanceComponent component;
    initializeMeshInstanceComponent(component, skinnedMesh);
    m_world->emplace<MeshInstanceComponent>(entity, std::move(component));

    SkinnedMeshComponent skinned;
    skinned.prototypeMesh = prototypeMesh;
    m_world->emplace<SkinnedMeshComponent>(entity, std::move(skinned));
}

void SceneEntityWorld::setSkinnedMeshReference(ecs::Entity entity, ecs::Entity skinnedMeshEntity)
{
    if (!ecs::isValid(skinnedMeshEntity))
        return;
    m_world->emplace<SkinnedMeshReferenceComponent>(entity, SkinnedMeshReferenceComponent{ skinnedMeshEntity });
}

void SceneEntityWorld::reconcileLightExclusivity(ecs::Entity entity)
{
    m_world->remove<DirectionalLightComponent>(entity);
    m_world->remove<SpotLightComponent>(entity);
    m_world->remove<PointLightComponent>(entity);
    m_world->remove<EnvironmentLightComponent>(entity);
}

void SceneEntityWorld::setDirectionalLight(ecs::Entity entity, DirectionalLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<DirectionalLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::setSpotLight(ecs::Entity entity, SpotLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<SpotLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::setPointLight(ecs::Entity entity, PointLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<PointLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::setEnvironmentLight(ecs::Entity entity, EnvironmentLightComponent component)
{
    reconcileLightExclusivity(entity);
    m_world->emplace<EnvironmentLightComponent>(entity, std::move(component));
}

void SceneEntityWorld::setCamera(ecs::Entity entity, CameraComponent component)
{
    m_world->emplace<CameraComponent>(entity, std::move(component));
}

void SceneEntityWorld::setAnimation(ecs::Entity entity, AnimationComponent component)
{
    m_world->emplace<AnimationComponent>(entity, std::move(component));
}

void SceneEntityWorld::setGaussianSplat(ecs::Entity entity, const GaussianSplat& splat)
{
    m_world->emplace<GaussianSplatComponent>(entity, GaussianSplatComponent{ splat });
}

void SceneEntityWorld::setSceneSettings(ecs::Entity entity, const SceneSettings& settings)
{
    m_world->emplace<SceneSettingsComponent>(entity, SceneSettingsComponent{ settings });
}

void SceneEntityWorld::setGameSettings(ecs::Entity entity, const GameSettings& settings)
{
    m_world->emplace<GameSettingsComponent>(entity, GameSettingsComponent{ settings });
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
            const auto* srcName = source.m_world->get<NameComponent>(srcEntity);
            const std::string name = srcName ? srcName->value : std::string{};
            ecs::Entity dstEntity = createEntity(name, dstParent);
            entityMap[srcEntity] = dstEntity;

            if (srcEntity == sourceRoot)
                newRoot = dstEntity;

            CopyEntityComponents(*m_world, dstEntity, *source.m_world, srcEntity, false);
            // Camera list / mesh ResourceTracker: syncSceneResourcesFromEcs() at import end.

            if (m_world->has<AnimationComponent>(dstEntity))
                importedAnimationEntities.push_back(dstEntity);

            const auto* srcMesh = source.m_world->get<MeshInstanceComponent>(srcEntity);
            const auto* srcSkinned = source.m_world->get<SkinnedMeshComponent>(srcEntity);
            if (srcMesh && srcMesh->mesh)
            {
                if (srcSkinned && srcSkinned->prototypeMesh && factory)
                {
                    setSkinnedMeshInstance(dstEntity, *factory, srcSkinned->prototypeMesh);

                    if (auto* dstMesh = m_world->get<MeshInstanceComponent>(dstEntity))
                    {
                        std::shared_ptr<MeshInfo> skinnedMesh = dstMesh->mesh;
                        *dstMesh = *srcMesh;
                        dstMesh->mesh = std::move(skinnedMesh);
                        // Importer-world indices are meaningless in the destination scene.
                        dstMesh->instanceIndex = -1;
                        dstMesh->geometryInstanceIndex = -1;
                    }

                    if (auto* dstSkinned = m_world->get<SkinnedMeshComponent>(dstEntity))
                    {
                        *dstSkinned = *srcSkinned;
                        dstSkinned->lastUpdateFrameIndex = 0;
                    }
                    importedSkinnedEntities.push_back(dstEntity);
                }
                else
                {
                    // Fresh MeshInstanceComponent — avoid copying stale -1 indices
                    // from the isolated importer world.
                    setMeshInstance(dstEntity, srcMesh->mesh);
                    if (srcSkinned)
                    {
                        SkinnedMeshComponent copiedSkinned = *srcSkinned;
                        copiedSkinned.lastUpdateFrameIndex = 0;
                        m_world->emplace<SkinnedMeshComponent>(dstEntity, std::move(copiedSkinned));
                        importedSkinnedEntities.push_back(dstEntity);
                    }
                }
            }

            if (const auto* children = source.m_world->get<ChildrenComponent>(srcEntity))
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
        auto* skinned = m_world->get<SkinnedMeshComponent>(skinnedEntity);
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
        auto* ref = m_world->get<SkinnedMeshReferenceComponent>(dstEntity);
        if (!ref || !ecs::isValid(ref->skinnedMeshEntity))
            continue;
        auto it = entityMap.find(ref->skinnedMeshEntity);
        if (it != entityMap.end())
            ref->skinnedMeshEntity = it->second;
    }

    m_world->each<MeshInstanceComponent>([&](ecs::Entity entity, MeshInstanceComponent& mesh) {
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
        auto* animation = m_world->get<AnimationComponent>(animEntity);
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
    syncSceneResourcesFromEcs();
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
            if (const auto* parent = m_world->get<ParentComponent>(current))
                current = parent->parent;
            else
                current = ecs::NullEntity;
            ++pathComponent;
            continue;
        }

        ecs::Entity found = ecs::NullEntity;
        if (const auto* children = m_world->get<ChildrenComponent>(current))
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
    if (const auto* path = m_world->get<PathComponent>(entity))
        return path->value;
    return {};
}

std::string SceneEntityWorld::getEntityName(ecs::Entity entity) const
{
    if (const auto* name = m_world->get<NameComponent>(entity))
        return name->value;
    return {};
}

const std::vector<ecs::Entity>& SceneEntityWorld::getEntityChildren(ecs::Entity entity) const
{
    if (const auto* children = m_world->get<ChildrenComponent>(entity))
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
