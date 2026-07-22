#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>
#include <render/core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <scene/SceneTypes.h>
#include <scene/SceneRenderData.h>

#include <render/core/ComputePass.h>
#include <shaders/SubInstanceData.h>
#include <shaders/PathTracer/Materials/StandardMaterial.h>

#include <unordered_map>


using namespace caustica::math;

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render
    {
    class RenderDevice;
    struct SceneGpuResources;
    }
    namespace scene
    {
    class SceneRenderData;
    }
    struct ImageAsset;
}

class ShaderDebug;
#include <scene/Scene.h>
class MaterialGpuCache;

enum class StandardMaterialTextureSlot
{
    Base,
    OcclusionRoughnessMetallic,
    Normal,
    Emissive,
    Transmission
};

struct MaterialShaderPermutation
{
    const std::string   shaderFilePath;

    const std::vector<caustica::ShaderMacro> 
                        macros;

    // runtime data
    int                 indexInTable            = -1;
    std::string         uniqueMaterialName      = "";
    std::string         stableShaderName        = "";
    int                 stableShaderId          = -1;
    uint32_t            featureTier             = 0;

    [[nodiscard]] caustica::ShaderKey makeShaderKey(
        nvrhi::GraphicsAPI api,
        ShaderCompilerUtils::ShaderProfile profile = ShaderCompilerUtils::ShaderProfile::Library_6_6) const;
};

struct MaterialShaderPermutationKey
{
    //    const std::array<unsigned char, 32/*picosha2::k_digest_size*/>  Hash;
    std::size_t                                                     hash;

    const std::string                                               fullKey;

    MaterialShaderPermutationKey( const MaterialShaderPermutation & msp );

    bool operator == (const MaterialShaderPermutationKey & other) const = default;    // technically we only need to compare fullKey
};

// Custom specialization of std::hash can be injected in namespace std.
struct MaterialShaderPermutationKeyHash
{
    // long hash to short hash
    std::size_t operator()(const MaterialShaderPermutationKey & s) const noexcept   { return s.hash; }
};

struct StandardMaterialTexture
{
    std::filesystem::path   localPath;
    bool                    sRGB = false;   // whether to assume that, when loading from sRGB agnostic formats, the texture's .rgb channels are in sRGB (.a is always linear)
    caustica::Handle<caustica::ImageAsset>
        loaded;
    bool                    normalMap = false; // determines unpacking (not actually used as a flag now by shading, but normalmaps are marked as so for future use)

    bool                    enabled = true; // an easy way to disable/enable texture slot without actually disconnecting a texture

    // float4                  ValueDefault;   // when texture is not enabled or can't be loaded
    // float4                  ValueMultiply;
    // float4                  ValueAdd;

    void                    initFromLoadedTexture(const caustica::Handle<caustica::ImageAsset>& loaded, bool sRGB, bool normalMap, const std::filesystem::path& mediaPath);
};

// All materials share these base properties and some of them have tight integration with the rest of the renderer
struct StandardMaterialBase
{
    MaterialGpuCache*          runtimeMaterialGpuCache = nullptr;

    // modelName + name is unique identifier for the material; there cannot be two materials with the same modelName and name - hopefully.
    std::string             name;
    std::string             modelName;                                  // material can be saved

    std::string             uniqueName;                                 // mix of Name and modelName but shortened and hashed - or whatever makes sense and is short

    // base texture? & alpha test - opacity?
    // emissive texture?

    bool                    sharedWithAllScenes                 = true;     // if 'true', will be saved to MaterialGpuCache::m_materialsPath; otherwise in MaterialGpuCache::m_materialsSceneSpecializedPath

    // this gets set internally by MaterialGpuCache::bakeShaderPermutations - it will be shared with other materials when possible
    std::shared_ptr<MaterialShaderPermutation>
                            bakedShaderPermutation;

    virtual void            write(Json::Value & output) = 0;
    virtual bool            read(
                                Json::Value& output,
                                const std::filesystem::path& mediaPath,
                                const std::shared_ptr<caustica::TextureLoader>& textureCache,
                                const std::filesystem::path& sceneDirectory = std::filesystem::path()) = 0;

    virtual bool            hasAlphaTest() const = 0;

    /*virtual*/ std::string     uniqueFullName()                            { return uniqueName; }

    virtual MaterialShaderPermutation computeShaderPermutation(const std::string & defaultShaderPath) = 0;
};                                                                                                                      

struct StandardMaterial : public StandardMaterialBase
{
    StandardMaterialTexture baseTexture;                        // .rgb base color; .a = opacity (both modes)
    StandardMaterialTexture occlusionRoughnessMetallicTexture;  // .rgb ORM; (spec-gloss fallback: specular color, .a = glossiness)
    StandardMaterialTexture normalTexture;
    StandardMaterialTexture emissiveTexture;
    StandardMaterialTexture transmissionTexture;                // see KHR_materials_transmission; undefined on specular-gloss materials

    dm::float3              baseOrDiffuseColor                  = 1.f; // metal-rough: base color, spec-gloss: diffuse color (if no texture present)
    dm::float3              specularColor                       = 1.f; // spec-gloss: specular color; OpenPBR: dielectric specular tint
    dm::float3              emissiveColor                       = 0.f;
    
    std::string             materialModel                       = "OpenPBR"; // Built-in material model. "RTXPT" keeps legacy naming only.
    float                   baseWeight                          = 1.f;
    float                   specularWeight                      = 1.f;
    float                   anisotropy                          = 0.f; // OpenPBR specular_roughness_anisotropy, range [-1, 1].
    float                   fuzzWeight                          = 0.f;
    dm::float3              fuzzColor                           = 1.f;
    float                   fuzzRoughness                       = 0.6f;

    // OpenPBR coat
    float                   coatWeight                          = 0.f;
    dm::float3              coatColor                           = 1.f;
    float                   coatRoughness                       = 0.f;
    float                   coatAnisotropy                      = 0.f;
    float                   coatIor                             = 1.6f;
    float                   coatDarkening                       = 1.f;

    // OpenPBR subsurface (dense scattering approx via lobe mix + volume sigmaS)
    float                   subsurfaceWeight                    = 0.f;
    dm::float3              subsurfaceColor                     = 1.f;
    float                   subsurfaceRadius                    = 1.f;
    float                   subsurfaceScale                     = 1.f;
    float                   subsurfaceAnisotropy                = 0.f;

    // OpenPBR thin-film
    float                   thinFilmWeight                      = 0.f;
    float                   thinFilmThickness                   = 0.5f; // micrometers
    float                   thinFilmIor                         = 1.4f;

    // OpenPBR transmission extras
    dm::float3              transmissionColor                   = 1.f;
    float                   transmissionDepth                   = 0.f;
    dm::float3              transmissionScatter                 = 0.f;
    float                   transmissionScatterAnisotropy       = 0.f;
    float                   transmissionDispersionScale         = 0.f;
    float                   transmissionDispersionAbbeNumber    = 20.f;

    float                   emissiveIntensity                   = 1.f; // additional multiplier for emissiveColor
    float                   metalness                           = 0.f; // metal-rough only
    float                   roughness                           = 0.f; // both metal-rough and spec-gloss
    float                   opacity                             = 1.f; // for transparent materials; multiplied by diffuse.a if present
    float                   transmissionFactor                  = 0.f; // see KHR_materials_transmission; undefined on specular-gloss materials
    float                   diffuseTransmissionFactor           = 0.f; // like specularTransmissionFactor, except using diffuse transmission lobe (roughness ignored)
    float                   normalTextureScale                  = 1.f;
    float                   IoR                                 = 1.5f; // index of refraction, see KHR_materials_ior

    // Toggle between two PBR models: metal-rough and specular-gloss.
    // See the comments on the other fields here.
    bool                    useSpecularGlossModel = false;

    // Toggles for the textures. Only effective if the corresponding texture is non-null.
    bool                    enableBaseTexture                   = true;
    bool                    enableOcclusionRoughnessMetallicTexture   = true;
    bool                    enableNormalTexture                 = true;
    bool                    enableEmissiveTexture               = true;
    bool                    enableTransmissionTexture           = true;

    bool                    enableAlphaTesting                  = false;
    float                   alphaCutoff                         = 0.5f; // for alpha tested materials

    bool                    enableTransmission                  = false;

    // Useful when metalness and roughness are packed into a 2-channel texture for BC5 encoding.
    bool                    metalnessInRedChannel               = false;

    // As per Falcor/RTXPT convention, ray hitting a material with the thin surface is assumed to enter and leave surface in the same bounce and it makes most sense when used with doubleSided; it skips all volume logic.
    bool                    thinSurface                         = false;

    // The mesh will not be part of NEE.
    bool                    excludeFromNEE                      = false;

    // will not propagate dominant stable plane when doing path space decomposition
    bool                    psdExclude                          = true;
    // for path space decomposition: -1 means no dominant; 0 usually means transmission, 1 usually means reflection, 2 usually means clearcoat reflection - must match corresponding BSDFSample::getDeltaLobeIndex()!
    int                     psdDominantDeltaLobe                = -1;
    // this surface is too curved or otherwise problematic to pass motion vectors; other decomposition can continue though
    int                     psdBlockMotionVectorsAtSurfaceType  = 0;        // 0 - Off; 1 - AutoLow; 2 - AutoHigh; 3 - Full

    // When volume meshes overlap, will cause higher nestedPriority mesh to 'carve out' the volumes with lower nestedPriority (see https://www.sidefx.com/docs/houdini/render/nested.html)
    static constexpr int kMaterialMaxNestedPriority = 14;
    int                     nestedPriority                      = kMaterialMaxNestedPriority;

    // KHR_materials_volume - see https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume#properties
    float                   volumeAttenuationDistance           = FLT_MAX;
    dm::float3              volumeAttenuationColor              = 1.0f;

    // Low tessellation geometry often has triangle (flat) normals that differ significantly from shading normals. This causes shading vs shadow discrepancy that exposes triangle edges. 
    // One way to mitigate this (other than having more detailed mesh) is to add additional shadowing falloff to hide the seam. 
    // This setting is not physically correct and adds bias. Setting of 0 means no fadeout (default).
    float                   shadowNoLFadeout                    = 0.0f;

    // Display the sampled base color without BRDF/lighting, but modulate it by light visibility.
    bool                    unlitReceiveShadows                 = false;
    float                   unlitShadowStrength                 = 1.0f;

    bool                    gpuDataDirty                        = true;         // params changed, GPU data needs update
    uint                    gpuDataIndex                        = 0xFFFFFFFF;   // 0xFFFFFFFF if no GPU buffer slot allocated

    bool                    enableAsAnalyticLightProxy          = false;

    bool                    ignoreMeshTangentSpace              = false;

    bool                    useEngineEmissiveIntensity           = false;        // for being able to use engine material animations
    
    bool                    skipRender                          = false;        // if 'true', we just skip drawing all geometries with this material; sometimes we can't edit a specific mesh but we can remove it this way; note: it can also be used for hidden emissives

    void                    fillData(StandardMaterialData & data);
    bool                    editorGui(class MaterialGpuCache & cache);
    bool                    isEmissive() const;
    StandardMaterialTexture& getTexture(StandardMaterialTextureSlot slot);
    const StandardMaterialTexture& getTexture(StandardMaterialTextureSlot slot) const;
    bool                    isTextureEnabled(StandardMaterialTextureSlot slot) const;
    void                    setTextureEnabled(StandardMaterialTextureSlot slot, bool enabled);

    static std::shared_ptr<StandardMaterial> safeCast(const std::shared_ptr<caustica::Material>& bridgeMaterial);

    static std::shared_ptr<StandardMaterial> fromJson(
        Json::Value& input,
        const std::filesystem::path& mediaPath,
        const std::shared_ptr<caustica::TextureLoader>& textureCache,
        const std::string& modelName,
        const std::string& name,
        const std::filesystem::path& sceneDirectory = std::filesystem::path());

    virtual void            write(Json::Value & output) override;
    virtual bool            read(
                                Json::Value& output,
                                const std::filesystem::path& mediaPath,
                                const std::shared_ptr<caustica::TextureLoader>& textureCache,
                                const std::filesystem::path& sceneDirectory = std::filesystem::path()) override;

    virtual MaterialShaderPermutation 
                            computeShaderPermutation(const std::string & defaultShaderPath) override;

    virtual bool            hasAlphaTest() const override       { return enableAlphaTesting; }
};

// Thin wrapper: adds path-tracing material data to the engine Material.
// StandardMaterial lives in the render layer, so this can't be merged into
// scene/SceneTypes.h without creating a layer dependency violation.
struct MaterialEx : caustica::Material
{
    std::shared_ptr<StandardMaterial> standardData;
};

class MaterialGpuCache
{
public:
    struct RayTracingState
    {
        bool skipRender = false;
        bool excludeFromNEE = false;
        bool alphaTest = false;
        bool transmission = false;
    };

    MaterialGpuCache(const std::string & relativeShaderSourcePath, nvrhi::IDevice* device, std::shared_ptr<caustica::TextureLoader> textureCache, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~MaterialGpuCache();

    void                            createRenderPassesAndLoadMaterials(nvrhi::IBindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials, const std::filesystem::path & sceneFilePath, const std::filesystem::path & mediaPath);

    // this update can happen in parallel with any other ray preparatory tracing work - anything from BVH building to laying down denoising layers
    void                            update(nvrhi::ICommandList* commandList,
                                           const caustica::scene::SceneRenderData& renderData,
                                           const caustica::render::SceneGpuResources* gpuResources,
                                           std::vector<SubInstanceData>& subInstanceData);

    nvrhi::BufferHandle             getMaterialDataBuffer() const           { return m_materialData; }
    uint                            getMaterialDataCount() const            { return m_materialsGPU.size(); }

    const std::unordered_map<std::string, StandardMaterialTexture> &
                                    getUsedTextures() const                 { return m_textures; }

    bool                            debugGui(float indent);

    void                            sceneReloaded();
    // Incrementally create StandardMaterial objects for scene materials that do not yet have
    // standardData. Used by runtime mesh import so existing materials stay valid.
    int                             ensureMaterialsFromScene(std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials);
    std::shared_ptr<StandardMaterial> findByResourceId(caustica::scene::MaterialRenderResourceId id) const;
    // Path-tracer pick feedback stores StandardMaterial::gpuDataIndex.
    std::shared_ptr<StandardMaterial> findByGpuDataIndex(uint gpuDataIndex) const;
    RayTracingState                 resolveRayTracingState(caustica::scene::MaterialRenderResourceId id) const;
    uint64_t                        materialStateRevision() const { return m_materialStateRevision; }
    void                            notifyMaterialEdited();

    std::filesystem::path           getMaterialStoragePath(StandardMaterialBase& material);

    const std::shared_ptr<MaterialShaderPermutation> & 
                                    getUbershader() const                   { return m_ubershader; }
    const std::vector<std::shared_ptr<MaterialShaderPermutation>> & 
                                    getShaderPermutationTable() const       { return m_shaderPermutationTable; }

    bool                            saveSingle(StandardMaterialBase& material);
    bool                            loadSingle(StandardMaterialBase& material);
    bool                            setMaterialTexture(
                                        StandardMaterial& material,
                                        StandardMaterialTextureSlot slot,
                                        const std::filesystem::path& localPath,
                                        std::optional<bool> sRGB = std::nullopt,
                                        std::optional<bool> normalMap = std::nullopt);
    void                            clearMaterialTexture(
                                        StandardMaterial& material,
                                        StandardMaterialTextureSlot slot);

    std::shared_ptr<StandardMaterial> findByUniqueId(const std::string & name);

private:
    void                            clear();

    std::shared_ptr<StandardMaterial> load(const std::string & modelFileName, const std::string& name);
    std::shared_ptr<StandardMaterial> importFromEngineMaterial(const caustica::scene::MaterialRenderResourceSnapshot& material);
    void                            saveAll();

    void                            completeDeferredTexturesLoad(nvrhi::ICommandList* commandList);
    void                            recordTexture(const StandardMaterialTexture& texture);
    bool                            reconcileLiveMaterials(std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials);
    void                            rebuildActiveTextureIndex();

    void                            bakeShaderPermutations();

    void                            initializeUniqueDeterministicName(const std::shared_ptr<StandardMaterialBase> & material);

private:
    nvrhi::DeviceHandle             m_device;
    std::string                     m_relativeShaderSourcePath;         // this is the path for the shader file containing material specializations for ClosestHit and (if enabled) AnyHit; it is currently 1 for all materials, but could be per-material
    std::shared_ptr<caustica::TextureLoader> m_textureCache;
    caustica::render::RenderDevice* m_renderDevice = nullptr;
    std::shared_ptr<caustica::FramebufferFactory> m_framebufferFactory;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    caustica::BindingCache     m_bindingCache;

    nvrhi::BufferHandle             m_materialData;
    bool                            m_materialDataWasReset = true;
    bool                            m_deferredTextureLoadInProgress = false;

    std::vector<std::shared_ptr<StandardMaterial>>
                                    m_materials;
    std::unordered_map<caustica::scene::MaterialRenderResourceId,
        std::shared_ptr<StandardMaterial>,
        caustica::scene::MaterialRenderResourceId::Hash> m_materialsById;
    uint64_t                        m_materialStateRevision = 1;
    std::vector<StandardMaterialData> m_materialsGPU;

    std::unordered_map<std::string, StandardMaterialTexture> m_textures;

    std::filesystem::path           m_mediaPath;
    std::filesystem::path           m_sceneDirectory;                     // parent directory of the loaded scene description file
    std::filesystem::path           m_sceneMaterialsPath;                 // <scene-dir>/Materials/
    std::filesystem::path           m_sceneMaterialsSceneSpecializedPath; // <scene-dir>/Materials/<scene-stem>/
    std::filesystem::path           m_materialsPath;                    // usually "Assets/Materials/"              <- used for materials shared between all scenes
    std::filesystem::path           m_materialsSceneSpecializedPath;    // usually "Assets/Materials/SceneName/"    <- used for materials specific to scene (not shared between scenes)

    // this ClosestHit/AnyHit should work for all materials (ubershader, not efficient) - it will likely be removed in the future, with only the specialized supported
    std::shared_ptr<MaterialShaderPermutation>
                                    m_ubershader;
    
    // these are per-material ClosestHit/AnyHit shaders (some materials share same); m_shaderPermutations and m_shaderPermutationTable are always updated together and point to same
    std::unordered_map<MaterialShaderPermutationKey, std::shared_ptr<MaterialShaderPermutation>, MaterialShaderPermutationKeyHash>
                                    m_shaderPermutations;
    std::vector<std::shared_ptr<MaterialShaderPermutation>> m_shaderPermutationTable;

    std::unordered_map<std::string, std::weak_ptr<StandardMaterialBase> > m_uniqueNames;
};
