#pragma once

#include <core/ThreadContext.h>
#include <ecs/Entity.h>
#include <ecs/World.h>
#include <ecs/Events.h>
#include <math/math.h>
#include <scene/SceneContent.h>
#include <scene/SceneResources.h>
#include <scene/SceneTypes.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace caustica
{
struct GaussianSplat;
struct SceneSettings;
struct GameSettings;
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

// Stable id from scene JSON `entities[].id`.
struct SceneAuthoringIdComponent
{
    std::string id;
};

// Root of an imported glTF/OBJ/USD/builtin prefab. Scene save writes this
// instead of the expanded import subtree.
struct PrefabInstanceComponent
{
    std::string source;
    std::unordered_map<std::string, std::string> materials;
};

// Explicit material asset on a mesh entity (or per imported slot).
struct MaterialOverrideComponent
{
    std::string source;
    std::unordered_map<std::string, std::string> slots;
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

    // Recomputes the cached matrix after translation / rotation / scaling change.
    void compose()
    {
        transform = dm::scaling(scaling);
        transform *= rotation.toAffine();
        transform *= dm::translation(translation);
    }

    // Authoring helper for Bundle spawn / system code.
    [[nodiscard]] static LocalTransformComponent fromTRS(
        const dm::double3& translation,
        const dm::dquat& rotation = dm::dquat::identity(),
        const dm::double3& scaling = dm::double3(1.0))
    {
        LocalTransformComponent local{};
        local.translation = translation;
        local.rotation = rotation;
        local.scaling = scaling;
        local.hasLocalTransform = true;
        local.compose();
        return local;
    }
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
    // Engine CPU mesh record. Apps: meshHandle() + MeshDeformApi(entity) / SceneSpawn / SceneTransform.
    // GPU keys are private on MeshInfo (scene::internal::RenderResourceAccess only).
    std::shared_ptr<MeshInfo> mesh;
    int instanceIndex = -1;
    int geometryInstanceIndex = -1;
    ecs::Entity proxiedAnalyticLight = ecs::NullEntity;
    // Hierarchy / Inspector visibility. Hidden instances keep their TLAS slot
    // (instanceMask = 0) so InstanceIndex stays aligned with ECS.
    bool enabled = true;

    [[nodiscard]] MeshHandle meshHandle() const
    {
        return mesh ? mesh->asset : MeshHandle{};
    }
};

inline constexpr uint32_t kForceSkinnedMeshUpdateFrameIndex = UINT32_MAX;

struct SkinnedMeshComponent
{
    std::shared_ptr<MeshInfo> prototypeMesh;
    std::vector<SkinnedMeshJoint> joints;
    uint32_t lastUpdateFrameIndex = 0;
    // One-shot request consumed by Extract. Used for loop wraps/seeks where the
    // previous rendered pose is not a valid temporal predecessor.
    bool resetMotionHistory = false;
};

struct SkinnedMeshReferenceComponent
{
    ecs::Entity skinnedMeshEntity = ecs::NullEntity;
};

// Typed payloads also used by LightRenderProxy (GPU-facing flat packet).
struct DirectionalLightData
{
    float irradiance = 1.f;
    float angularSize = 0.f;
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

// A one-sided rectangular emitter in the local XY plane, facing local -Z.
// Intensity is emitted radiance, matching an emissive rectangle material.
struct RectLightData
{
    float intensity = 1.f;
    float width = 1.f;
    float height = 1.f;
};

struct EnvironmentLightData
{
    dm::float3 radianceScale = dm::float3(1.f);
    int textureIndex = -1;
    float rotation = 0.f;
    std::string path;
};

using LightData = std::variant<DirectionalLightData, SpotLightData, PointLightData, RectLightData, EnvironmentLightData>;

// UE-style typed light components (mutually exclusive on an entity).
struct DirectionalLightComponent
{
    bool enabled = true;
    dm::float3 color = dm::colors::white;
    float irradiance = 1.f;
    float angularSize = 0.f;
};

struct SpotLightComponent
{
    bool enabled = true;
    dm::float3 color = dm::colors::white;
    std::vector<std::string> proxies;
    float intensity = 1.f;
    float radius = 0.f;
    float range = 0.f;
    float innerAngle = 180.f;
    float outerAngle = 180.f;
};

struct PointLightComponent
{
    bool enabled = true;
    dm::float3 color = dm::colors::white;
    std::vector<std::string> proxies;
    float intensity = 1.f;
    float radius = 0.f;
    float range = 0.f;
};

struct RectLightComponent
{
    bool enabled = true;
    dm::float3 color = dm::colors::white;
    float intensity = 1.f;
    float width = 1.f;
    float height = 1.f;
};

struct EnvironmentLightComponent
{
    bool enabled = true;
    dm::float3 color = dm::colors::white;
    dm::float3 radianceScale = dm::float3(1.f);
    int textureIndex = -1;
    float rotation = 0.f;
    std::string path;
};

struct CameraIntrinsics
{
    float fx = 0.f;
    float fy = 0.f;
    float cx = 0.f;
    float cy = 0.f;
    float width = 0.f;
    float height = 0.f;
};

struct PerspectiveCameraData
{
    float zNear = 1.f;
    float verticalFov = 1.f;
    std::optional<float> zFar;
    std::optional<float> aspectRatio;
    std::optional<bool> enableAutoExposure;
    std::optional<std::string> toneMapOperator;
    std::optional<float> exposureCompensation;
    std::optional<float> exposureValue;
    std::optional<float> exposureValueMin;
    std::optional<float> exposureValueMax;
    // When set, overrides symmetric verticalFov with an off-center pinhole.
    std::optional<CameraIntrinsics> intrinsics;
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
    // Editor-authored tracks are serialized back into scene JSON by the editor.
    // Imported animation components remain owned by their source assets.
    bool editorAuthored = false;
};

// Fixed-topology mesh point cache (e.g. soft body from USD bake).
struct GeometrySequenceComponent
{
    std::shared_ptr<MeshInfo> mesh;
    uint32_t vertexCount = 0;
    std::vector<float> timesSeconds;
    // Interleaved frames: frameCount * vertexCount * 3
    std::vector<float> positions;
    int lastAppliedFrameA = -1;
    int lastAppliedFrameB = -1;
    float lastAppliedAlpha = -1.f;
    // After a pose upload, PrevPosition still encodes the previous keyframe.
    // Sync it to the current pose on the next held display frame so TAA/NRD/DLSS
    // do not keep seeing stale inter-keyframe motion while the mesh is static.
    bool prevPositionsNeedSync = false;
    bool recomputeNormals = true;
    // Physics caches are authored per discrete time sample. Sub-frame lerp at display
    // refresh rates causes continuous AS updates and temporal-filter thrash.
    bool interpolateFrames = false;
};

struct GaussianSplatComponent
{
    GaussianSplat splat;
};

struct SceneSettingsComponent
{
    SceneSettings settings;
};

struct GameSettingsComponent
{
    GameSettings settings;
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

void updateHierarchy(ecs::World& world, PreviousTransformPolicy previousPolicy);

[[nodiscard]] SceneContentFlags getMeshContentFlags(const MeshInfo& mesh);
[[nodiscard]] dm::box3 getMeshLocalBounds(const MeshInfo& mesh);
[[nodiscard]] bool setMeshProperty(MeshInfo& mesh, const std::string& propName, const dm::float4& value);
void initializeMeshInstanceComponent(MeshInstanceComponent& component, const std::shared_ptr<MeshInfo>& mesh);
[[nodiscard]] std::shared_ptr<MeshInfo> createSkinnedMeshFromPrototype(
    SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh);

// ECS scene world: entity hierarchy + resource tracking (meshes, lights, cameras, ...).
class SceneEntityWorld : public SceneResources
{
public:
    // Scratch / tests / async pending Scene: owns a registry.
    SceneEntityWorld();
    // Live scene: borrows App::m_world so Query / Res / Commands share one registry.
    explicit SceneEntityWorld(ecs::World& liveWorld);
    ~SceneEntityWorld() override;

    SceneEntityWorld(const SceneEntityWorld&) = delete;
    SceneEntityWorld& operator=(const SceneEntityWorld&) = delete;

    [[nodiscard]] bool ownsRegistry() const { return m_owned != nullptr; }

    // Drop the scene graph without touching ecs resources (Time, plugins, ...).
    void resetScene();

    // Graft this scratch registry into `liveWorld` (logic thread, after async load).
    // Rebinds this object to `liveWorld` and releases the owned registry.
    void adoptInto(ecs::World& liveWorld, SceneTypeFactory* factory = nullptr);

    void refreshHierarchy(PreviousTransformPolicy previousPolicy = PreviousTransformPolicy::CaptureCurrent);
    // Align previous-frame transforms with current (avoids bogus motion after scene load).
    void syncPreviousTransformsFromCurrent();
    void refresh(uint32_t frameIndex);
    void clear();

    ecs::Entity createEntity(const std::string& name = {}, ecs::Entity parent = ecs::NullEntity);
    void destroyEntity(ecs::Entity entity);
    bool setParent(ecs::Entity entity, ecs::Entity parent);

    // Bevy-style bundle spawn: createEntity defaults + plain component emplace.
    // Mesh/light/camera resource lists sync in syncSceneResourcesFromEcs()
    // (beginRefreshFrame / ensureSceneResourcesSynced) from Added<>/Changed<>.
    template<typename... Components>
    ecs::Entity spawn(Components&&... components)
    {
        return spawnNamed({}, ecs::NullEntity, std::forward<Components>(components)...);
    }

    template<typename... Components>
    ecs::Entity spawnNamed(const std::string& name, ecs::Entity parent, Components&&... components)
    {
        ecs::Entity entity = createEntity(name, parent);
        (insertSpawnComponent(entity, std::forward<Components>(components)), ...);
        return entity;
    }

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
    void setDirectionalLight(ecs::Entity entity, DirectionalLightComponent component);
    void setSpotLight(ecs::Entity entity, SpotLightComponent component);
    void setPointLight(ecs::Entity entity, PointLightComponent component);
    void setRectLight(ecs::Entity entity, RectLightComponent component);
    void setEnvironmentLight(ecs::Entity entity, EnvironmentLightComponent component);
    void setCamera(ecs::Entity entity, CameraComponent component);
    void setAnimation(ecs::Entity entity, AnimationComponent component);
    void setGaussianSplat(ecs::Entity entity, const GaussianSplat& splat);
    void setSceneSettings(ecs::Entity entity, const SceneSettings& settings);
    void setGameSettings(ecs::Entity entity, const GameSettings& settings);

    // Deep-copies a subtree from another world into this one under `parent`.
    ecs::Entity importSubtree(
        ecs::Entity parent,
        const SceneEntityWorld& source,
        ecs::Entity sourceRoot,
        SceneTypeFactory* factory = nullptr);

    void applyAnimations(float time);
    void markTransformDirty();
    // createEntity/setParent dirties structure via Parent/Children. Analytic
    // lights are render proxies, not GPU geometry — drop that bit when no mesh
    // / camera / animation / splat actually changed this frame.
    void discardStructureDirtyIfGeometryUnchanged();
    void markSkinnedMeshDirtyForJoint(ecs::Entity jointEntity);
    void resetSkinnedMeshMotionHistory();
    void assignGlobalResourceIndices();
    void refreshInstanceIndices();

    [[nodiscard]] ecs::World& world()
    {
        assertLogicThread();
        assert(m_world);
        return *m_world;
    }
    [[nodiscard]] const ecs::World& world() const
    {
        assertLogicThread();
        assert(m_world);
        return *m_world;
    }

    [[nodiscard]] ecs::Entity root() const { return m_root; }
    [[nodiscard]] ecs::Entity entityForPath(const std::filesystem::path& path) const;
    [[nodiscard]] ecs::Entity findEntity(const std::filesystem::path& path, ecs::Entity context = ecs::NullEntity) const;
    [[nodiscard]] std::filesystem::path getEntityPath(ecs::Entity entity) const;
    [[nodiscard]] std::string getEntityName(ecs::Entity entity) const;
    [[nodiscard]] const std::vector<ecs::Entity>& getEntityChildren(ecs::Entity entity) const;
    [[nodiscard]] bool entitySubtreeContains(ecs::Entity root, ecs::Entity candidate) const;

    [[nodiscard]] bool hasPendingStructureChanges();
    [[nodiscard]] bool hasPendingTransformChanges();
    [[nodiscard]] bool hasPendingLightChanges();

    // Reconcile ResourceTracker / camera list / leaf bounds from Added<>/Changed<>.
    // Called from beginRefreshFrame; also via ensureSceneResourcesSynced() before mesh reads.
    void syncSceneResourcesFromEcs();
    void ensureSceneResourcesSynced();

    // Advance ChangeDetection after all Changed<> readers for this logic frame
    // (Extract). refresh()/finalizeRefreshFrame() must NOT end the tick.
    void endChangeDetectionFrame();

    // ChangeDetection is the component write signal. m_*Dirty are refresh/Extract
    // caches hydrated from ChangeDetection — not a second host-facing dirty API.

    [[nodiscard]] const std::vector<ecs::Entity>& cameraEntitiesInRegistrationOrder() const;

private:
    void registerCameraEntity(ecs::Entity entity);
    void unregisterCameraEntity(ecs::Entity entity);
    void unregisterEntityLeaves(ecs::Entity entity);
    void updateLeafContentAndBounds(ecs::Entity entity);
    void ensureChangeDetection();
    void syncDirtyFlagsFromChangeDetection();
    void reconcileLightExclusivity(ecs::Entity entity);

    void insertSpawnComponent(ecs::Entity entity, NameComponent component);
    void insertSpawnComponent(ecs::Entity entity, LocalTransformComponent component);
    void insertSpawnComponent(ecs::Entity entity, MeshInstanceComponent component);
    void insertSpawnComponent(ecs::Entity entity, DirectionalLightComponent component);
    void insertSpawnComponent(ecs::Entity entity, SpotLightComponent component);
    void insertSpawnComponent(ecs::Entity entity, PointLightComponent component);
    void insertSpawnComponent(ecs::Entity entity, RectLightComponent component);
    void insertSpawnComponent(ecs::Entity entity, EnvironmentLightComponent component);
    void insertSpawnComponent(ecs::Entity entity, CameraComponent component);
    void insertSpawnComponent(ecs::Entity entity, AnimationComponent component);
    void insertSpawnComponent(ecs::Entity entity, GaussianSplatComponent component);

    template<typename T>
    void insertSpawnComponent(ecs::Entity entity, T&& component)
    {
        m_world->emplace<std::remove_cvref_t<T>>(entity, std::forward<T>(component));
    }

    void beginRefreshFrame();
    void markDirtySkinnedMeshes(uint32_t frameIndex);
    void markDirtySkinnedMeshesFromChangedJoints(uint32_t frameIndex);
    void applyDeferredCommands();
    void refreshInstanceIndicesIfNeeded();
    void assignGlobalResourceIndicesIfNeeded();
    void finalizeRefreshFrame();

    std::unique_ptr<ecs::World> m_owned;
    ecs::World* m_world = nullptr;
    bool m_frameStructureDirty = false;   // per-frame snapshot of m_structureDirty for systems
    bool m_frameTransformDirty = false;   // per-frame snapshot of m_transformDirty for systems
    bool m_frameLightDirty = false;       // survives refresh until Extract publishes the light list
    ecs::Entity m_root = ecs::NullEntity;
    std::vector<ecs::Entity> m_CameraEntities;
    std::unordered_map<std::string, ecs::Entity> m_pathToEntity;
    // Shadow of ResourceTracker registrations keyed by entity (for Changed mesh swaps).
    std::unordered_map<ecs::Entity, std::shared_ptr<MeshInfo>> m_registeredMeshByEntity;
    bool m_structureDirty = true;         // refresh/Extract cache (from ChangeDetection)
    bool m_transformDirty = true;         // refresh/Extract cache (from ChangeDetection)
    bool m_lightDirty = true;             // refresh only analytic/environment light proxies
    bool m_suppressStructureDirtyForLightOnlyEdit = false;
    bool m_previousTransformDirty = false;
    static inline const std::vector<ecs::Entity> s_emptyChildren{};
};

} // namespace caustica::scene
