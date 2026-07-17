#pragma once

#include <math/math.h>
#include <assets/Handle.h>
#include <assets/ImageAsset.h>
#include <assets/TypedAssets.h>
#include <scene/SceneRenderResourceIds.h>
#include <shaders/light_types.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct MaterialConstants;
struct LightConstants;

namespace Json
{
    class Value;
}

namespace caustica
{
    class IBlob;
}

namespace caustica
{
    enum class VertexAttribute
    {
        Position,
        PrevPosition,
        TexCoord1,
        TexCoord2,
        Normal,
        Tangent,
        Transform,
        PrevTransform,
        JointIndices,
        JointWeights,
        CurveRadius,

        Count
    };

    nvrhi::VertexAttributeDesc getVertexAttributeDesc(VertexAttribute attribute, const char* name, uint32_t bufferIndex);


    struct SceneLoadingStats
    {
        std::atomic<uint32_t> ObjectsTotal;
        std::atomic<uint32_t> ObjectsLoaded;
    };

    // NOTE regarding MaterialDomain and transparency. It may seem that the Transparent attribute
    // is orthogonal to the blending mode (opaque, alpha-tested, alpha-blended). In glTF, it is
    // indeed an independent extension, KHR_materials_transmission, that can interact with the
    // blending mode. But enabling physical transmission on an object is an important change
    // for renderers: for example, rasterizers need to render "opaque" transmissive objects in a
    // separate render pass, together with alpha blended materials; ray tracers also need to
    // process transmissive objects in a different way from regular opaque or alpha-tested objects.
    // Specifying the transmission option in the material domain makes these requirements explicit.

    enum class MaterialDomain : uint8_t
    {
        Opaque,
        AlphaTested,
        AlphaBlended,
        Transmissive,
        TransmissiveAlphaTested,
        TransmissiveAlphaBlended,

        Count
    };

    const char* materialDomainToString(MaterialDomain domain);

    struct Material
    {
        scene::MaterialRenderResourceId renderResourceId;
        Handle<MaterialAsset> asset;
        std::string name;
        std::string modelFileName;      // where this material originated from, e.g. GLTF file name
        int materialIndexInModel = -1;  // index of the material in the model file
        MaterialDomain domain = MaterialDomain::Opaque;
        Handle<ImageAsset> baseOrDiffuseTexture; // metal-rough: base color; spec-gloss: diffuse color; .a = opacity (both modes)
        Handle<ImageAsset> metalRoughOrSpecularTexture; // metal-rough: ORM map; spec-gloss: specular color, .a = glossiness
        Handle<ImageAsset> normalTexture;
        Handle<ImageAsset> emissiveTexture;
        Handle<ImageAsset> occlusionTexture;
        Handle<ImageAsset> transmissionTexture; // see KHR_materials_transmission; undefined on specular-gloss materials
        Handle<ImageAsset> opacityTexture; // for renderers that store opacity or alpha mask separately, overrides baseOrDiffuse.a
        dm::float3 baseOrDiffuseColor = 1.f; // metal-rough: base color, spec-gloss: diffuse color (if no texture present)
        dm::float3 specularColor = 0.f; // spec-gloss: specular color
        dm::float3 emissiveColor = 0.f;
        float emissiveIntensity = 1.f; // additional multiplier for emissiveColor
        float metalness = 0.f; // metal-rough only
        float roughness = 0.f; // both metal-rough and spec-gloss
        float opacity = 1.f; // for transparent materials; multiplied by diffuse.a if present
        float alphaCutoff = 0.5f; // for alpha tested materials
        float transmissionFactor = 0.f; // see KHR_materials_transmission; undefined on specular-gloss materials
        float normalTextureScale = 1.f;
        float occlusionStrength = 1.f;
        dm::float2 normalTextureTransformScale = 1.f;

        // Toggle between two PBR models: metal-rough and specular-gloss.
        // See the comments on the other fields here.
        bool useSpecularGlossModel = false;

        // Subsurface Scattering
        bool enableSubsurfaceScattering = false;
        struct SubsurfaceParams
        {
            dm::float3 transmissionColor = 0.5f;
            dm::float3 scatteringColor = 0.5f;
            float scale = 1.0f;
            float anisotropy = 0.0f;
        } subsurface;

        // Hair
        bool enableHair = false;
        struct HairParams
        {
            dm::float3 baseColor = 1.0f;
            float melanin = 0.5f;
            float melaninRedness = 0.5f;
            float longitudinalRoughness = 0.25f;
            float azimuthalRoughness = 0.6f;
            float diffuseReflectionWeight = 0.0f;
            dm::float3 diffuseReflectionTint = 0.0f;
            float ior = 1.55f;
            float cuticleAngle = 3.0f;
        } hair;

        // Toggles for the textures. Only effective if the corresponding texture is non-null.
        bool enableBaseOrDiffuseTexture = true;
        bool enableMetalRoughOrSpecularTexture = true;
        bool enableNormalTexture = true;
        bool enableEmissiveTexture = true;
        bool enableOcclusionTexture = true;
        bool enableTransmissionTexture = true;
        bool enableOpacityTexture = true;

        bool doubleSided = false;
        
        // Useful when metalness and roughness are packed into a 2-channel texture for BC5 encoding.
        bool metalnessInRedChannel = false;

        int materialID = 0;

        virtual ~Material() = default;
        void fillConstantBuffer(struct MaterialConstants& constants, bool useResourceDescriptorHeapBindless = false) const;
        bool setProperty(const std::string& name, const dm::float4& value);
    };


    struct InputAssemblerBindings
    {
        VertexAttribute vertexBuffers[16];
        uint32_t numVertexBuffers;
    };

    struct BufferGroup
    {
        std::vector<uint32_t> indexData;
        std::vector<dm::float3> positionData;
        std::vector<dm::float2> texcoord1Data;
        std::vector<dm::float2> texcoord2Data;
        std::vector<uint32_t> normalData;
        std::vector<uint32_t> tangentData;
        std::vector<dm::vector<uint16_t, 4>> jointData;
        std::vector<dm::float4> weightData;
        std::vector<float> radiusData;
        std::vector<dm::float4> morphTargetData;
    };

    enum class MeshGeometryPrimitiveType : uint8_t
    {
        Triangles,
        Lines,
        LineStrip,

        Count
    };

    struct MeshGeometry
    {
        scene::GeometryRenderResourceId renderResourceId;
        std::shared_ptr<Material> material;
        dm::box3 objectSpaceBounds;
        uint32_t indexOffsetInMesh = 0;
        uint32_t vertexOffsetInMesh = 0;
        uint32_t numIndices = 0;
        uint32_t numVertices = 0;
        int globalGeometryIndex = 0;

        MeshGeometryPrimitiveType type = MeshGeometryPrimitiveType::Triangles;

        virtual ~MeshGeometry() = default;
    };

    enum class MeshType : uint8_t
    {
        Triangles,
        CurvePolytubes,
        CurveDisjointOrthogonalTriangleStrips,
        CurveLinearSweptSpheres,

        Count
    };

    struct MeshInfo
    {
        scene::MeshRenderResourceId renderResourceId;
        Handle<MeshAsset> asset;
        std::string name;
        MeshType type = MeshType::Triangles;
        std::shared_ptr<BufferGroup> buffers;
        std::shared_ptr<MeshInfo> skinPrototype;
        std::vector<std::shared_ptr<MeshGeometry>> geometries;
        dm::box3 objectSpaceBounds;
        uint32_t indexOffset = 0;
        uint32_t vertexOffset = 0;
        uint32_t totalIndices = 0;
        uint32_t totalVertices = 0;
        int globalMeshIndex = 0;
        bool isMorphTargetAnimationMesh = false;
        bool isSkinPrototype = false;

        std::vector<uint32_t> DeformationSourcePositionIndices; // preserves OBJ v-order for deformation APIs

        virtual ~MeshInfo() = default;
        bool isCurve() const
        {
            return (type == MeshType::CurvePolytubes)
                || (type == MeshType::CurveDisjointOrthogonalTriangleStrips)
                || (type == MeshType::CurveLinearSweptSpheres);
        }
    };

    inline nvrhi::IBuffer* bufferOrFallback(nvrhi::IBuffer* primary, nvrhi::IBuffer* secondary)
    {
        return primary ? primary : secondary;
    }

    namespace detail
    {
        template <typename T>
        concept HasCpuOwnedMeshAccelStruct = requires(T value) { value.accelStruct; };
        template <typename T>
        concept HasCpuOwnedMeshOmm = requires(T value) { value.AccelStructOMM; };
        template <typename T>
        concept HasCpuOwnedMeshDirty = requires(T value) { value.DebugDataDirty; };

        template <typename T>
        concept HasCpuOwnedVertexBuffer = requires(T value) { value.vertexBuffer; };
        template <typename T>
        concept HasCpuOwnedDescriptor = requires(T value) { value.indexBufferDescriptor; };
        template <typename T>
        concept HasCpuOwnedBufferRanges = requires(T value) { value.vertexBufferRanges; };

        template <typename T>
        concept HasCpuOwnedMaterialConstants = requires(T value) { value.materialConstants; };
        template <typename T>
        concept HasCpuOwnedMaterialDirty = requires(T value) { value.dirty; };
    }

    static_assert(!detail::HasCpuOwnedMeshAccelStruct<MeshInfo>
            && !detail::HasCpuOwnedMeshOmm<MeshInfo>
            && !detail::HasCpuOwnedMeshDirty<MeshInfo>,
        "MeshInfo must remain CPU-only; GPU state belongs in render::MeshGpuRecord.");
    static_assert(!detail::HasCpuOwnedVertexBuffer<BufferGroup>
            && !detail::HasCpuOwnedDescriptor<BufferGroup>
            && !detail::HasCpuOwnedBufferRanges<BufferGroup>,
        "BufferGroup must remain CPU staging-only; GPU handles belong to Render.");
    static_assert(!detail::HasCpuOwnedMaterialConstants<Material>
            && !detail::HasCpuOwnedMaterialDirty<Material>,
        "Material must remain authoring-only; GPU buffers and dirty state belong to Render.");
}
