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

    // Per-light and per-geometry link into the light sampling system.
    // Render-side caches own history; do not treat these as game-thread authority.
    struct LightSamplerLink
    {
        int IndexOrBase = -1;   // index of the corresponding PolymorphicLight in the light sampler
        int LastUpdateTag = -1; // identifier of when IndexOrBase was last updated
    };

    // Joint reference for skinned meshes; uses an ECS entity instead of a scene graph node.
    struct SkinnedMeshJoint
    {
        ecs::Entity jointEntity = ecs::NullEntity;
        dm::float4x4 inverseBindMatrix = dm::float4x4::identity();
    };

    // =========================================================================
    // GaussianSplat / SampleSettings / GameSettings — value payloads on ECS
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

    struct SampleSettings
    {
        std::string name;
        std::optional<bool>  realtimeMode;
        std::optional<bool>  enableAnimations;
        std::optional<int>   startingCamera;
        std::optional<float> realtimeFireflyFilter;
        std::optional<int>   maxBounces;
        std::optional<int>   maxDiffuseBounces;
        std::optional<float> textureMIPBias;

        void load(const Json::Value& node);
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
