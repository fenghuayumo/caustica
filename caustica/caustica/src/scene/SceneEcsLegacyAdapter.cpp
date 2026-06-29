#include <scene/SceneEcsLegacyAdapter.h>

#include <scene/SceneGraph.h>

namespace caustica::scene
{

namespace
{
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

void AttachLegacyLeafComponents(SceneEntityWorld& entityWorld, ecs::Entity entity, const std::shared_ptr<SceneGraphLeaf>& leaf)
{
    if (!leaf)
        return;

    auto& world = entityWorld.world();
    world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{ leaf->GetLocalBoundingBox() });
    world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = leaf->GetContentFlags(),
        .subgraphContent = leaf->GetContentFlags(),
    });

    if (auto mesh = std::dynamic_pointer_cast<MeshInstance>(leaf))
        world.emplace<MeshInstanceComponent>(entity, MeshInstanceComponent{ mesh });
    if (auto skinnedMesh = std::dynamic_pointer_cast<SkinnedMeshInstance>(leaf))
        world.emplace<SkinnedMeshInstanceComponent>(entity, SkinnedMeshInstanceComponent{ skinnedMesh });
    if (auto light = std::dynamic_pointer_cast<Light>(leaf))
        world.emplace<LightComponent>(entity, LightComponent{ light });
    if (auto camera = std::dynamic_pointer_cast<SceneCamera>(leaf))
        world.emplace<CameraComponent>(entity, CameraComponent{ camera });
    if (auto splat = std::dynamic_pointer_cast<GaussianSplat>(leaf))
        world.emplace<GaussianSplatComponent>(entity, GaussianSplatComponent{ splat });
    if (auto sampleSettings = std::dynamic_pointer_cast<SampleSettings>(leaf))
        world.emplace<SampleSettingsComponent>(entity, SampleSettingsComponent{ sampleSettings });
    if (auto gameSettings = std::dynamic_pointer_cast<GameSettings>(leaf))
        world.emplace<GameSettingsComponent>(entity, GameSettingsComponent{ gameSettings });
}

ecs::Entity ImportLegacyNode(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraphNode>& node, ecs::Entity parent)
{
    ecs::Entity entity = entityWorld.createEntity(node->GetName(), parent);
    entityWorld.setPath(entity, node->GetPath());

    auto& world = entityWorld.world();
    world.emplace<LocalTransformComponent>(entity, MakeLocalTransform(*node));
    world.emplace<GlobalTransformComponent>(entity, MakeGlobalTransform(*node));
    world.emplace<LocalBoundsComponent>(entity, LocalBoundsComponent{});
    world.emplace<BoundsComponent>(entity, BoundsComponent{ node->GetGlobalBoundingBox() });
    world.emplace<SceneContentComponent>(entity, SceneContentComponent{
        .leafContent = node->GetLeafContentFlags(),
        .subgraphContent = node->GetSubgraphContentFlags(),
    });

    AttachLegacyLeafComponents(entityWorld, entity, node->GetLeaf());

    for (size_t index = 0; index < node->GetNumChildren(); ++index)
        ImportLegacyNode(entityWorld, node->GetChild(index)->shared_from_this(), entity);

    return entity;
}

bool SyncLegacyNode(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraphNode>& node)
{
    ecs::Entity entity = entityWorld.entityForPath(node->GetPath());
    if (!ecs::isValid(entity))
        return false;

    auto& world = entityWorld.world();
    if (auto* name = world.get<NameComponent>(entity))
        name->value = node->GetName();
    entityWorld.setPath(entity, node->GetPath());
    if (auto* local = world.get<LocalTransformComponent>(entity))
        *local = MakeLocalTransform(*node);
    if (auto* global = world.get<GlobalTransformComponent>(entity))
        *global = MakeGlobalTransform(*node);
    if (auto* bounds = world.get<BoundsComponent>(entity))
        bounds->globalBounds = node->GetGlobalBoundingBox();
    if (auto* content = world.get<SceneContentComponent>(entity))
    {
        content->leafContent = node->GetLeafContentFlags();
        content->subgraphContent = node->GetSubgraphContentFlags();
    }

    const auto* children = world.get<ChildrenComponent>(entity);
    if (!children || children->children.size() != node->GetNumChildren())
        return false;

    for (size_t index = 0; index < node->GetNumChildren(); ++index)
    {
        if (!SyncLegacyNode(entityWorld, node->GetChild(index)->shared_from_this()))
            return false;
    }

    return true;
}
} // namespace

void RebuildWorldFromLegacyScene(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraph>& legacyScene)
{
    entityWorld.clear();

    if (!legacyScene || !legacyScene->GetRootNode())
        return;

    ImportLegacyNode(entityWorld, legacyScene->GetRootNode(), ecs::NullEntity);
    entityWorld.refreshHierarchy(PreviousTransformPolicy::PreserveExisting);
}

void SyncWorldFromLegacyScene(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraph>& legacyScene)
{
    if (!legacyScene || !legacyScene->GetRootNode())
    {
        entityWorld.clear();
        return;
    }

    if (!ecs::isValid(entityWorld.root()) || !SyncLegacyNode(entityWorld, legacyScene->GetRootNode()))
    {
        RebuildWorldFromLegacyScene(entityWorld, legacyScene);
        return;
    }

    entityWorld.refreshHierarchy(PreviousTransformPolicy::PreserveExisting);
}

} // namespace caustica::scene
