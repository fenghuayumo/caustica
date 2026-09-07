#pragma once

#include <scene/SceneContent.h>
#include <scene/SceneTypes.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Json { class Value; }

namespace caustica
{
    class SceneTypeFactory;

    // Joint reference for skinned meshes; uses an ECS entity instead of a scene graph node.
    struct SkinnedMeshJoint
    {
        ecs::Entity jointEntity = ecs::NullEntity;
        math::float4x4 inverseBindMatrix = math::float4x4::identity();
    };

    // =========================================================================
    // GaussianSplat / SceneSettings / GameSettings — value payloads on ECS
    // =========================================================================

    struct GaussianSplat
    {
        std::string name;
        std::string path;
        std::string resolvedPath;
        bool convertRdfToRub = true;
        bool enabled = true;
        uint32_t loadedSplatCount = 0;

        void load(const Json::Value& node);
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }
    };

    // Inspector environment look (PathTracerSettings.EnvironmentMapParams + env override).
    // Each field is optional so load applies only keys that were actually authored.
    struct EnvironmentLookSettings
    {
        std::optional<math::float3> tintColor;
        std::optional<float> intensity;
        std::optional<math::float3> rotationXYZ;
        std::optional<bool> visibleToCamera;
        std::optional<bool> enabled;
        std::optional<std::string> overrideSource;
    };

    // Inspector 3DGS look (PathTracerSettings GaussianSplat* session fields).
    struct GaussianSplatLookSettings
    {
        std::optional<float> footprintScale;
        std::optional<float> alphaScale;
        std::optional<float> brightness;
        std::optional<math::float3> tintColor;
        std::optional<bool> applyToneMapping;
        std::optional<bool> asEmitter;
        std::optional<float> emissionIntensity;
        std::optional<float> alphaCullThreshold;
        std::optional<float> shadowStrength;
    };

    struct SceneSettings
    {
        std::string name;
        std::optional<bool>  realtimeMode;
        std::optional<bool>  enableAnimations;
        std::optional<bool>  enableKeyframes;
        std::optional<int>   startingCamera;
        std::optional<std::string> startingCameraId;
        std::optional<float> realtimeFireflyFilter;
        std::optional<int>   maxBounces;
        std::optional<int>   maxDiffuseBounces;
        std::optional<float> textureMIPBias;
        std::optional<EnvironmentLookSettings> environment;
        std::optional<GaussianSplatLookSettings> gaussianSplat;
        // Paths of currently hidden mesh / splat / light entities. Absent or empty
        // means "do not change visibility" — never hide the rest of the scene.
        std::vector<std::string> hiddenEntities;

        void load(const Json::Value& node);
        void writeLook(Json::Value& settingsNode) const;
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }
    };

    struct GameSettings
    {
        std::string name;
        std::string jsonData;

        void load(const Json::Value& node);
        [[nodiscard]] const std::string& getJsonData() const { return jsonData; }
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::None; }
    };

    // =========================================================================
    // SceneTypeFactory
    // =========================================================================

    // Factory that creates mesh/material/JSON leaf payloads. Subclasses may override
    // to produce project-specific subtypes (e.g. MaterialEx).
    class SceneTypeFactory
    {
    public:
        virtual ~SceneTypeFactory() = default;

        // Returns a type-erased object by type string; caller casts via static_pointer_cast.
        // Returns nullptr for unrecognised or unsupported types.
        // Cameras/lights are built as ECS components (see SceneComponentBuilders), not here.
        virtual std::shared_ptr<void> createLeaf(const std::string& type);

        virtual std::shared_ptr<Material>     createMaterial();
        virtual std::shared_ptr<MeshInfo>     createMesh();
        virtual std::shared_ptr<MeshGeometry> createMeshGeometry();
    };

} // namespace caustica
