#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneGraph.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace caustica::scene
{

struct NameComponent
{
    std::string value;
};

struct ParentComponent
{
    ecs::Entity parent;
};

struct ChildrenComponent
{
    std::vector<ecs::Entity> children;
};

struct SceneRootResource
{
    ecs::Entity root = ecs::NullEntity;
};

struct LocalTransformComponent
{
    dm::double3 translation = 0.0;
    dm::dquat rotation = dm::dquat::identity();
    dm::double3 scaling = 1.0;
    dm::daffine3 transform = dm::daffine3::identity();
    bool hasLocalTransform = false;
};

struct GlobalTransformComponent
{
    dm::daffine3 transform = dm::daffine3::identity();
    dm::affine3 transformFloat = dm::affine3::identity();
    dm::daffine3 previousTransform = dm::daffine3::identity();
    dm::affine3 previousTransformFloat = dm::affine3::identity();
};

struct BoundsComponent
{
    dm::box3 globalBounds = dm::box3::empty();
};

struct SceneContentComponent
{
    SceneContentFlags leafContent = SceneContentFlags::None;
    SceneContentFlags subgraphContent = SceneContentFlags::None;
};

struct TransformDirtyComponent
{
    bool value = true;
};

struct HierarchyDirtyComponent
{
    bool value = true;
};

struct SceneGraphNodeComponent
{
    std::weak_ptr<SceneGraphNode> node;
};

struct SceneLeafComponent
{
    std::shared_ptr<SceneGraphLeaf> leaf;
    SceneContentFlags contentFlags = SceneContentFlags::None;
};

struct MeshInstanceComponent
{
    std::shared_ptr<MeshInstance> instance;
};

struct SkinnedMeshInstanceComponent
{
    std::shared_ptr<SkinnedMeshInstance> instance;
};

struct LightComponent
{
    std::shared_ptr<Light> light;
};

struct CameraComponent
{
    std::shared_ptr<SceneCamera> camera;
};

struct AnimationComponent
{
    std::shared_ptr<SceneGraphAnimation> animation;
};

struct GaussianSplatComponent
{
    std::shared_ptr<GaussianSplat> splat;
};

struct SampleSettingsComponent
{
    std::shared_ptr<SampleSettings> settings;
};

struct GameSettingsComponent
{
    std::shared_ptr<GameSettings> settings;
};

struct SceneGraphDirtyComponent
{
    SceneGraphNode::DirtyFlags flags = SceneGraphNode::DirtyFlags::None;
    SceneContentFlags subgraphContent = SceneContentFlags::None;
};

enum class PreviousTransformPolicy
{
    CaptureCurrent,
    PreserveExisting
};

void UpdateHierarchy(ecs::World& world, PreviousTransformPolicy previousPolicy);

class SceneEntityWorld
{
public:
    void rebuildFromSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph);
    void syncTransformsFromSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph);
    void refreshHierarchy(PreviousTransformPolicy previousPolicy = PreviousTransformPolicy::CaptureCurrent);
    void clear();

    ecs::Entity createEntity(const std::string& name = {}, ecs::Entity parent = ecs::NullEntity);
    void destroyEntity(ecs::Entity entity);
    bool setParent(ecs::Entity entity, ecs::Entity parent);

    void setLocalTransform(ecs::Entity entity,
        const dm::double3* translation,
        const dm::dquat* rotation,
        const dm::double3* scaling);
    void setTranslation(ecs::Entity entity, const dm::double3& translation);
    void setRotation(ecs::Entity entity, const dm::dquat& rotation);
    void setScaling(ecs::Entity entity, const dm::double3& scaling);

    [[nodiscard]] ecs::World& world() { return m_world; }
    [[nodiscard]] const ecs::World& world() const { return m_world; }

    [[nodiscard]] ecs::Entity root() const { return m_root; }
    [[nodiscard]] ecs::Entity entityForNode(const SceneGraphNode* node) const;
    [[nodiscard]] std::shared_ptr<SceneGraphNode> nodeForEntity(ecs::Entity entity) const;

private:
    ecs::Entity importNode(const std::shared_ptr<SceneGraphNode>& node, ecs::Entity parent);
    void syncNodeRecursive(const std::shared_ptr<SceneGraphNode>& node);
    void attachLeafComponents(ecs::Entity entity, const std::shared_ptr<SceneGraphLeaf>& leaf);

    ecs::World m_world;
    ecs::Entity m_root = ecs::NullEntity;
    std::unordered_map<const SceneGraphNode*, ecs::Entity> m_nodeToEntity;
};

} // namespace caustica::scene
