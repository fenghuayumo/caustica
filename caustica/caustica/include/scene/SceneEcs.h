#pragma once

#include <ecs/Entity.h>
#include <ecs/Schedule.h>
#include <ecs/World.h>
#include <ecs/Events.h>
#include <math/math.h>
#include <rhi/nvrhi.h>
#include <scene/SceneContent.h>
#include <scene/SceneResources.h>
#include <scene/SceneTypes.h>

#include <cstdint>
#include <memory>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace caustica
{
class Light;
class SceneCamera;
class SceneAnimation;
class GaussianSplat;
class SampleSettings;
class GameSettings;
class SceneTypeFactory;
struct SkinnedMeshJoint;
}

namespace caustica::scene
{

struct NameComponent
{
    std::string value;
};

struct PathComponent
{
    std::filesystem::path value;
};

struct ParentComponent
{
    ecs::Entity parent = ecs::NullEntity;
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

struct LocalBoundsComponent
{
    dm::box3 bounds = dm::box3::empty();
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

struct MeshInstanceComponent
{
    std::shared_ptr<MeshInfo> mesh;
    int instanceIndex = -1;
    int geometryInstanceIndex = -1;
    std::vector<LightSamplerLink> perGeometryLightSamplerLinks;
    ecs::Entity proxiedAnalyticLight = ecs::NullEntity;
};

inline constexpr uint32_t kForceSkinnedMeshUpdateFrameIndex = UINT32_MAX;

struct SkinnedMeshComponent
{
    std::shared_ptr<MeshInfo> prototypeMesh;
    std::vector<SkinnedMeshJoint> joints;
    uint32_t lastUpdateFrameIndex = 0;
};

struct SkinnedMeshGpuComponent
{
    nvrhi::BufferHandle jointBuffer;
    nvrhi::BindingSetHandle skinningBindingSet;
    bool skinningInitialized = false;
};

struct SkinnedMeshReferenceComponent
{
    ecs::Entity skinnedMeshEntity = ecs::NullEntity;
};

struct DirectionalLightData
{
    float irradiance = 1.f;
    float angularSize = 0.f;
    std::vector<std::shared_ptr<caustica::IShadowMap>> perObjectShadows;
};

struct SpotLightData
{
    float intensity = 1.f;
    float radius = 0.f;
    float range = 0.f;
    float innerAngle = 180.f;
    float outerAngle = 180.f;
};

struct PointLightData
{
    float intensity = 1.f;
    float radius = 0.f;
    float range = 0.f;
};

struct EnvironmentLightData
{
    dm::float3 radianceScale = dm::float3(1.f);
    int textureIndex = -1;
    float rotation = 0.f;
    std::string path;
};

using LightData = std::variant<DirectionalLightData, SpotLightData, PointLightData, EnvironmentLightData>;

struct LightComponent
{
    std::shared_ptr<caustica::IShadowMap> shadowMap;
    int shadowChannel = -1;
    dm::float3 color = dm::colors::white;
    LightSamplerLink lightLink;
    std::vector<std::string> proxies;
    LightData data;
};

struct PerspectiveCameraData
{
    float zNear = 1.f;
    float verticalFov = 1.f;
    std::optional<float> zFar;
    std::optional<float> aspectRatio;
    std::optional<bool> enableAutoExposure;
    std::optional<float> exposureCompensation;
    std::optional<float> exposureValue;
    std::optional<float> exposureValueMin;
    std::optional<float> exposureValueMax;
};

struct OrthographicCameraData
{
    float zNear = 0.f;
    float zFar = 1.f;
    float xMag = 1.f;
    float yMag = 1.f;
};

using CameraData = std::variant<PerspectiveCameraData, OrthographicCameraData>;

struct CameraComponent
{
    CameraData data;
};

struct AnimationChannelData
{
    std::shared_ptr<caustica::animation::Sampler> sampler;
    ecs::Entity targetEntity = ecs::NullEntity;
    std::shared_ptr<caustica::Material> targetMaterial;
    caustica::AnimationAttribute attribute = caustica::AnimationAttribute::Undefined;
    std::string leafPropertyName;
};

struct AnimationComponent
{
    std::vector<AnimationChannelData> channels;
    float duration = 0.f;
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

struct TransformChangedEvent
{
    ecs::Entity entity = ecs::NullEntity;
};

enum class PreviousTransformPolicy
{
    CaptureCurrent,
    PreserveExisting
};

void UpdateHierarchy(ecs::World& world, PreviousTransformPolicy previousPolicy);

// ECS scene world: entity hierarchy + resource tracking (meshes, lights, cameras, …).
class SceneEntityWorld : public SceneResources
{
public:
    void refreshHierarchy(PreviousTransformPolicy previousPolicy = PreviousTransformPolicy::CaptureCurrent);
    void refresh(uint32_t frameIndex);
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
    void setPath(ecs::Entity entity, const std::filesystem::path& path);
    void rebuildPathsFromRoot();

    void setMeshInstance(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh);
    void setSkinnedMeshInstance(ecs::Entity entity, SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh);
    void setSkinnedMeshReference(ecs::Entity entity, ecs::Entity skinnedMeshEntity);
    void setLight(ecs::Entity entity, LightComponent component);
    void setLight(ecs::Entity entity, const std::shared_ptr<Light>& light);
    void setCamera(ecs::Entity entity, CameraComponent component);
    void setCamera(ecs::Entity entity, const std::shared_ptr<SceneCamera>& camera);
    void setAnimation(ecs::Entity entity, AnimationComponent component);
    void setAnimation(ecs::Entity entity, const std::shared_ptr<SceneAnimation>& animation);
    void setGaussianSplat(ecs::Entity entity, const std::shared_ptr<GaussianSplat>& splat);
    void setSampleSettings(ecs::Entity entity, const std::shared_ptr<SampleSettings>& settings);
    void setGameSettings(ecs::Entity entity, const std::shared_ptr<GameSettings>& settings);

    // Deep-copies a subtree from another world into this one under `parent`.
    ecs::Entity importSubtree(
        ecs::Entity parent,
        const SceneEntityWorld& source,
        ecs::Entity sourceRoot,
        SceneTypeFactory* factory = nullptr);

    void applyAnimations(float time);
    void markTransformDirty();
    void markSkinnedMeshDirtyForJoint(ecs::Entity jointEntity);
    void assignGlobalResourceIndices();
    void refreshInstanceIndices();

    [[nodiscard]] ecs::World& world() { return m_world; }
    [[nodiscard]] const ecs::World& world() const { return m_world; }

    [[nodiscard]] ecs::Entity root() const { return m_root; }
    [[nodiscard]] ecs::Entity entityForPath(const std::filesystem::path& path) const;
    [[nodiscard]] ecs::Entity findEntity(const std::filesystem::path& path, ecs::Entity context = ecs::NullEntity) const;
    [[nodiscard]] std::filesystem::path getEntityPath(ecs::Entity entity) const;
    [[nodiscard]] std::string getEntityName(ecs::Entity entity) const;
    [[nodiscard]] const std::vector<ecs::Entity>& getEntityChildren(ecs::Entity entity) const;
    [[nodiscard]] bool entitySubtreeContains(ecs::Entity root, ecs::Entity candidate) const;

    [[nodiscard]] bool hasPendingStructureChanges() const { return m_structureDirty; }
    [[nodiscard]] bool hasPendingTransformChanges() const { return m_transformDirty || m_previousTransformDirty; }

    [[nodiscard]] const std::vector<ecs::Entity>& cameraEntitiesInRegistrationOrder() const { return m_CameraEntities; }

private:
    void registerCameraEntity(ecs::Entity entity);
    void unregisterCameraEntity(ecs::Entity entity);
    void unregisterEntityLeaves(ecs::Entity entity);
    void updateLeafContentAndBounds(ecs::Entity entity);
    void ensureChangeDetection();
    void syncDirtyFlagsFromChangeDetection();

    // Phase-1 ECS schedule: the CPU scene refresh is a sequence of ordered systems
    // driven by ecs::Schedule. Each systemXxx is one step split out of refresh().
    void ensureScheduleBuilt();
    void systemRefreshHierarchy(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemUpdateGaussianSplatTransforms(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemMarkDirtySkinnedMeshes(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemMarkDirtySkinnedMeshesFromChangedJoints(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemApplyDeferredCommands(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemRefreshInstanceIndices(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemAssignGlobalResourceIndices(ecs::World& world, const ecs::ScheduleContext& ctx);
    void systemFinalizeFrameFlags(ecs::World& world, const ecs::ScheduleContext& ctx);

    ecs::World m_world;
    ecs::Schedule m_schedule;
    bool m_scheduleBuilt = false;
    bool m_frameStructureDirty = false;   // per-frame snapshot of m_structureDirty for systems
    bool m_frameTransformDirty = false;   // per-frame snapshot of m_transformDirty for systems
    ecs::Entity m_root = ecs::NullEntity;
    std::vector<ecs::Entity> m_CameraEntities;
    std::unordered_map<std::string, ecs::Entity> m_pathToEntity;
    bool m_structureDirty = true;
    bool m_transformDirty = true;
    bool m_previousTransformDirty = false;
    static inline const std::vector<ecs::Entity> s_emptyChildren{};
};

} // namespace caustica::scene
