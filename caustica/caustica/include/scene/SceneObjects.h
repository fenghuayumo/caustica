#pragma once

#include <scene/SceneContent.h>
#include <scene/SceneTypes.h>
#include <scene/IShadowMap.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct LightConstants;

namespace Json { class Value; }

namespace caustica::scene { class SceneEntityWorld; }

namespace caustica
{
    class Light;
    class SceneTypeFactory;

    // Per-light and per-geometry link into the light sampling system.
    struct LightSamplerLink
    {
        int IndexOrBase = -1;   // index of the corresponding PolymorphicLight in the light sampler
        int LastUpdateTag = -1; // identifier of when IndexOrBase was last updated
    };

    // =========================================================================
    // MeshInstance
    // =========================================================================

    class MeshInstance
    {
        friend class SceneResources;
        int m_InstanceIndex = -1;
        int m_GeometryInstanceIndex = -1;

    protected:
        std::shared_ptr<MeshInfo> m_Mesh;

    public:
        std::string name;
        ecs::Entity ownerEntity = ecs::NullEntity; // entity whose GlobalTransformComponent provides the world transform

        explicit MeshInstance(std::shared_ptr<MeshInfo> mesh)
            : m_Mesh(std::move(mesh))
        {
            if (m_Mesh)
                PerGeometryLightSamplerLinks.resize(m_Mesh->geometries.size(), { -1, -1 });
        }

        virtual ~MeshInstance() = default;

        [[nodiscard]] const std::shared_ptr<MeshInfo>& getMesh() const { return m_Mesh; }
        [[nodiscard]] int getInstanceIndex() const { return m_InstanceIndex; }
        [[nodiscard]] int getGeometryInstanceIndex() const { return m_GeometryInstanceIndex; }
        [[nodiscard]] virtual dm::box3 getLocalBoundingBox() const { return m_Mesh ? m_Mesh->objectSpaceBounds : dm::box3::empty(); }
        [[nodiscard]] virtual std::shared_ptr<MeshInstance> clone() const;
        [[nodiscard]] SceneContentFlags getContentFlags() const;
        bool setProperty(const std::string& propName, const dm::float4& value);

        std::vector<LightSamplerLink> PerGeometryLightSamplerLinks;
        std::weak_ptr<Light> ProxiedAnalyticLight;

        MeshInstance(const MeshInstance&) = delete;
        MeshInstance(MeshInstance&&) = delete;
        MeshInstance& operator=(const MeshInstance&) = delete;
        MeshInstance& operator=(MeshInstance&&) = delete;
    };

    // =========================================================================
    // SkinnedMeshInstance / SkinnedMeshReference
    // =========================================================================

    // Joint reference for skinned meshes; uses an ECS entity instead of a scene graph node.
    struct SkinnedMeshJoint
    {
        ecs::Entity jointEntity = ecs::NullEntity;
        dm::float4x4 inverseBindMatrix = dm::float4x4::identity();
    };

    class SkinnedMeshInstance : public MeshInstance
    {
        friend class SceneResources;

        std::shared_ptr<MeshInfo> m_PrototypeMesh;
        uint32_t m_LastUpdateFrameIndex = 0;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        std::vector<SkinnedMeshJoint> joints;
        nvrhi::BufferHandle jointBuffer;
        nvrhi::BindingSetHandle skinningBindingSet;
        bool skinningInitialized = false;

        explicit SkinnedMeshInstance(std::shared_ptr<SceneTypeFactory> sceneTypeFactory, std::shared_ptr<MeshInfo> prototypeMesh);

        [[nodiscard]] const std::shared_ptr<MeshInfo>& getPrototypeMesh() const { return m_PrototypeMesh; }
        [[nodiscard]] uint32_t getLastUpdateFrameIndex() const { return m_LastUpdateFrameIndex; }
        void setLastUpdateFrameIndex(uint32_t frameIndex) { m_LastUpdateFrameIndex = frameIndex; }
        [[nodiscard]] std::shared_ptr<MeshInstance> clone() const override;
    };

    // Attached to joint entities so that when the joint transform changes the skinned mesh is flagged for rebuild.
    // The skeleton hierarchy may be separate from the mesh entity.
    class SkinnedMeshReference
    {
        friend class SceneResources;
        std::weak_ptr<SkinnedMeshInstance> m_Instance;

    public:
        std::string name;

        explicit SkinnedMeshReference(std::shared_ptr<SkinnedMeshInstance> instance)
            : m_Instance(std::move(instance))
        {}

        [[nodiscard]] std::shared_ptr<SkinnedMeshInstance> getInstance() const { return m_Instance.lock(); }
        [[nodiscard]] std::shared_ptr<SkinnedMeshReference> clone() const;
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }

        SkinnedMeshReference(const SkinnedMeshReference&) = delete;
        SkinnedMeshReference& operator=(const SkinnedMeshReference&) = delete;
    };

    // =========================================================================
    // SceneCamera / PerspectiveCamera / OrthographicCamera
    // =========================================================================

    class SceneCamera
    {
    public:
        std::string name;
        ecs::Entity ownerEntity = ecs::NullEntity;
        mutable dm::daffine3 cachedGlobalTransform = dm::daffine3::identity();

        virtual ~SceneCamera() = default;

        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::Cameras; }

        // Callers supply the entity's GlobalTransformComponent::transform.
        [[nodiscard]] dm::affine3 getViewToWorldMatrix(const dm::daffine3& globalTransform) const
        {
            return dm::scaling(dm::float3(1.f, 1.f, -1.f)) * dm::affine3(globalTransform);
        }
        [[nodiscard]] dm::affine3 getWorldToViewMatrix(const dm::daffine3& globalTransform) const
        {
            return dm::affine3(inverse(globalTransform)) * dm::scaling(dm::float3(1.f, 1.f, -1.f));
        }

        [[nodiscard]] dm::affine3 getViewToWorldMatrix() const
        {
            return getViewToWorldMatrix(cachedGlobalTransform);
        }
        [[nodiscard]] dm::affine3 getWorldToViewMatrix() const
        {
            return getWorldToViewMatrix(cachedGlobalTransform);
        }

        virtual void load(const Json::Value& node) {}
        virtual bool setProperty(const std::string& propName, const dm::float4& value) { return false; }

    protected:
        SceneCamera() = default;

        SceneCamera(const SceneCamera&) = delete;
        SceneCamera(SceneCamera&&) = delete;
        SceneCamera& operator=(const SceneCamera&) = delete;
        SceneCamera& operator=(SceneCamera&&) = delete;
    };

    class PerspectiveCamera : public SceneCamera
    {
    public:
        float zNear = 1.f;
        float verticalFov = 1.f; // radians
        std::optional<float> zFar; // reverse infinite projection when absent
        std::optional<float> aspectRatio;

        // Auto-exposure / tone mapping
        std::optional<bool>  enableAutoExposure;
        std::optional<float> exposureCompensation;
        std::optional<float> exposureValue;
        std::optional<float> exposureValueMin;
        std::optional<float> exposureValueMax;

        [[nodiscard]] std::shared_ptr<PerspectiveCamera> clone() const;
        void load(const Json::Value& node) override;
        bool setProperty(const std::string& propName, const dm::float4& value) override;
    };

    class OrthographicCamera : public SceneCamera
    {
    public:
        float zNear = 0.f;
        float zFar = 1.f;
        float xMag = 1.f;
        float yMag = 1.f;

        [[nodiscard]] std::shared_ptr<OrthographicCamera> clone() const;
        void load(const Json::Value& node) override;
        bool setProperty(const std::string& propName, const dm::float4& value) override;
    };

    // =========================================================================
    // Light hierarchy
    // =========================================================================

    class Light
    {
    public:
        std::string name;
        ecs::Entity ownerEntity = ecs::NullEntity;
        mutable dm::daffine3 cachedGlobalTransform = dm::daffine3::identity();
        std::shared_ptr<IShadowMap> shadowMap;
        int shadowChannel = -1;
        dm::float3 color = dm::colors::white;

        // Light sampler integration
        LightSamplerLink LightLink;
        std::vector<std::string> Proxies; // proxy mesh entity names for light sampling

        virtual ~Light() = default;

        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::Lights; }

        [[nodiscard]] virtual int getLightType() const = 0;
        // Caller supplies the entity's GlobalTransformComponent::transform.
        virtual void fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const;
        virtual void store(Json::Value& node) const {}
        virtual void load(const Json::Value& node) {}
        virtual bool setProperty(const std::string& propName, const dm::float4& value);

        // Getters derive position/direction from the supplied global transform.
        [[nodiscard]] dm::double3 getPosition(const dm::daffine3& globalTransform) const
        {
            return globalTransform.m_translation;
        }
        [[nodiscard]] dm::double3 getDirection(const dm::daffine3& globalTransform) const
        {
            return -normalize(dm::double3(globalTransform.m_linear.row2));
        }

        [[nodiscard]] dm::double3 getPosition() const { return getPosition(cachedGlobalTransform); }
        [[nodiscard]] dm::double3 getDirection() const { return getDirection(cachedGlobalTransform); }

        void fillLightConstants(LightConstants& lightConstants) const
        {
            fillLightConstants(lightConstants, cachedGlobalTransform);
        }

        void updateCachedDirection(const dm::double3& direction)
        {
            const dm::double3 position = cachedGlobalTransform.m_translation;
            cachedGlobalTransform = lookatZ(direction);
            cachedGlobalTransform.m_translation = position;
        }

        // Setters update the entity's local transform via SceneEntityWorld.
        void setPosition(scene::SceneEntityWorld& world, ecs::Entity entity, const dm::double3& position) const;
        void setDirection(scene::SceneEntityWorld& world, ecs::Entity entity, const dm::double3& direction) const;

    protected:
        Light() = default;

        Light(const Light&) = delete;
        Light(Light&&) = delete;
        Light& operator=(const Light&) = delete;
        Light& operator=(Light&&) = delete;
    };

    class DirectionalLight : public Light
    {
    public:
        float irradiance = 1.f;  // target illuminance (lm/m²) multiplied by color
        float angularSize = 0.f; // angular diameter of the source, in degrees
        std::vector<std::shared_ptr<IShadowMap>> perObjectShadows;

        [[nodiscard]] std::shared_ptr<DirectionalLight> clone() const;
        [[nodiscard]] int getLightType() const override { return LightType_Directional; }
        using Light::fillLightConstants;
        void fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const override;
        void load(const Json::Value& node) override;
        void store(Json::Value& node) const override;
        bool setProperty(const std::string& propName, const dm::float4& value) override;
    };

    class SpotLight : public Light
    {
    public:
        float intensity = 1.f;   // luminous intensity (lm/sr) multiplied by color
        float radius = 0.f;      // sphere radius, in world units
        float range = 0.f;       // influence range; 0 = infinite
        float innerAngle = 180.f; // apex angle of the full-bright cone, in degrees
        float outerAngle = 180.f; // apex angle of the full cone, in degrees

        [[nodiscard]] std::shared_ptr<SpotLight> clone() const;
        [[nodiscard]] int getLightType() const override { return LightType_Spot; }
        using Light::fillLightConstants;
        void fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const override;
        void load(const Json::Value& node) override;
        void store(Json::Value& node) const override;
        bool setProperty(const std::string& propName, const dm::float4& value) override;
    };

    class PointLight : public Light
    {
    public:
        float intensity = 1.f; // luminous intensity (lm/sr) multiplied by color
        float radius = 0.f;    // sphere radius, in world units
        float range = 0.f;     // influence range; 0 = infinite

        [[nodiscard]] std::shared_ptr<PointLight> clone() const;
        [[nodiscard]] int getLightType() const override { return LightType_Point; }
        using Light::fillLightConstants;
        void fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const override;
        void load(const Json::Value& node) override;
        void store(Json::Value& node) const override;
        bool setProperty(const std::string& propName, const dm::float4& value) override;
    };

    // Environment / sky light
    class EnvironmentLight : public Light
    {
    public:
        dm::float3 radianceScale = dm::float3(1.f);
        int textureIndex = -1;
        float rotation = 0.f;
        std::string path;

        [[nodiscard]] std::shared_ptr<EnvironmentLight> clone() const;
        [[nodiscard]] int getLightType() const override { return LightType_Environment; }
        using Light::fillLightConstants;
        void fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const override;
        void load(const Json::Value& node) override;
    };

    // =========================================================================
    // GaussianSplat / SampleSettings / GameSettings
    // =========================================================================

    class GaussianSplat
    {
    public:
        std::string name;
        ecs::Entity ownerEntity = ecs::NullEntity;
        mutable dm::daffine3 cachedGlobalTransform = dm::daffine3::identity();
        std::string path;
        std::string resolvedPath;
        bool convertRdfToRub = true;
        bool enabled = true;
        uint32_t loadedSplatCount = 0;

        [[nodiscard]] std::shared_ptr<GaussianSplat> clone() const;
        void load(const Json::Value& node);
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }

        GaussianSplat() = default;
        GaussianSplat(const GaussianSplat&) = delete;
        GaussianSplat& operator=(const GaussianSplat&) = delete;
    };

    class SampleSettings
    {
    public:
        std::string name;
        std::optional<bool>  realtimeMode;
        std::optional<bool>  enableAnimations;
        std::optional<int>   startingCamera;
        std::optional<float> realtimeFireflyFilter;
        std::optional<int>   maxBounces;
        std::optional<int>   maxDiffuseBounces;
        std::optional<float> textureMIPBias;

        [[nodiscard]] std::shared_ptr<SampleSettings> clone() const;
        void load(const Json::Value& node);
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }

        SampleSettings() = default;
        SampleSettings(const SampleSettings&) = delete;
        SampleSettings& operator=(const SampleSettings&) = delete;
    };

    class GameSettings
    {
        std::string m_JsonData;

    public:
        std::string name;

        [[nodiscard]] std::shared_ptr<GameSettings> clone() const;
        void load(const Json::Value& node);
        [[nodiscard]] const std::string& getJsonData() const { return m_JsonData; }
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }

        GameSettings() = default;
        GameSettings(const GameSettings&) = delete;
        GameSettings& operator=(const GameSettings&) = delete;
    };

    // =========================================================================
    // SceneTypeFactory
    // =========================================================================

    // Factory that creates typed scene objects. Subclasses may override any method
    // to produce project-specific subtypes.
    // Placed in SceneObjects.h (not SceneResources.h) to break the circular dependency
    // with SkinnedMeshInstance, which needs SceneTypeFactory in its constructor.
    class SceneTypeFactory
    {
    public:
        virtual ~SceneTypeFactory() = default;

        // Returns a type-erased object by type string; caller casts via static_pointer_cast.
        // Returns nullptr for unrecognised or unsupported types.
        virtual std::shared_ptr<void> createLeaf(const std::string& type);

        virtual std::shared_ptr<Material>            createMaterial();
        virtual std::shared_ptr<MeshInfo>            createMesh();
        virtual std::shared_ptr<MeshGeometry>        createMeshGeometry();
        virtual std::shared_ptr<MeshInstance>        createMeshInstance(const std::shared_ptr<MeshInfo>& mesh);
        virtual std::shared_ptr<SkinnedMeshInstance> createSkinnedMeshInstance(
            const std::shared_ptr<SceneTypeFactory>& sceneTypeFactory,
            const std::shared_ptr<MeshInfo>& prototypeMesh);
    };

} // namespace caustica
