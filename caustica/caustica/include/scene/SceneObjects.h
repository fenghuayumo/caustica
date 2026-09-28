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
        std::optional<float> alphaCullThreshold;
        std::optional<float> shadowStrength;
        std::optional<bool> secondaryRays;
        std::optional<bool> illuminateMeshes;
        std::optional<float> radianceAlphaClamp;
    };

    // RenderSettings panel fields beyond the SceneSettings top-level scalars.
    // Each field is optional so load applies only keys that were authored.
    struct RenderSettingsLook
    {
        std::optional<int>  realtimeSamplesPerPixel;
        std::optional<int>  accumulationTarget;
        std::optional<int>  realtimeAA;
        std::optional<int>  dlssMode;
        std::optional<bool> standaloneDenoiser;
        std::optional<bool> referenceOidnDenoiser;
        std::optional<bool> referenceOidnUseGpu;
        std::optional<bool> useNEE;
        std::optional<int>  neeType;
        std::optional<int>  neeCandidateSamples;
        std::optional<int>  neeFullSamples;
        std::optional<bool> useRestirDI;
        std::optional<bool> useRestirGI;
        std::optional<bool> useRestirPT;
        std::optional<int>  restirPreset;    // RTXDIRestirQualityPreset as int
        std::optional<int>  restirPTPreset;  // RTXDIRestirPTQualityPreset as int
        std::optional<bool> realtimeFireflyEnabled;
        std::optional<float> realtimeFireflyThreshold;
        std::optional<bool> referenceFireflyEnabled;
        std::optional<float> referenceFireflyThreshold;
        std::optional<int>  nestedDielectricsQuality;
        // Quality-performance preset side effects (no direct UI control).
        std::optional<int>  neeMISType;
        std::optional<int>  environmentMapDiffuseSampleMIPLevel;
        std::optional<int>  stablePlanesActiveCount;
        std::optional<bool> allowPrimarySurfaceReplacement;
        std::optional<bool> enableLDSamplerForBSDF;
        // Advanced Gaussian Splats section.
        std::optional<int>  gaussianSplatPrimaryMethod;
        std::optional<int>  gaussianSplatShadowsMode;
        std::optional<int>  gaussianSplatSortingMode;
        std::optional<int>  gaussianSplatSHFormat;
        std::optional<bool> gaussianSplatQuantizeNormals;
        std::optional<bool> gaussianSplatMipAntialiasing;
        std::optional<int>  gaussianSplatFrustumCulling;
        std::optional<bool> gaussianSplatScreenSizeCulling;
        std::optional<float> gaussianSplatMinPixelCoverage;
    };

    // Post Process panel fields (bloom / tone mapping / late LDR).
    struct PostProcessLook
    {
        std::optional<bool>  bloomEnabled;
        std::optional<float> bloomRadius;
        std::optional<float> bloomIntensity;
        std::optional<bool>  toneMappingEnabled;
        std::optional<int>   toneMapOperator;   // ToneMapperOperator as int
        std::optional<bool>  autoExposure;
        std::optional<int>   exposureMode;      // ExposureMode as int
        std::optional<float> exposureCompensation;
        std::optional<float> exposureValue;
        std::optional<float> exposureValueMin;
        std::optional<float> exposureValueMax;
        std::optional<float> filmSpeed;
        std::optional<float> fNumber;
        std::optional<float> shutter;
        std::optional<bool>  whiteBalance;
        std::optional<float> whitePoint;
        std::optional<float> whiteMaxLuminance;
        std::optional<float> whiteScale;
        std::optional<bool>  whiteClamped;
        std::optional<bool>  cameraLutEnabled;
        std::optional<bool>  cameraLutAfterToneMap;
        std::optional<int>   cameraLutPreset;   // CameraLutPreset as int
        std::optional<std::string> cameraLutPath;
        std::optional<bool>  edgeDetection;
        std::optional<float> edgeDetectionThreshold;
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
        std::optional<RenderSettingsLook> renderSettings;
        std::optional<PostProcessLook> postProcess;
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
