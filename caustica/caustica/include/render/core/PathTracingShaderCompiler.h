#pragma once

#include <mutex>
#include <memory>
#include <rhi/nvrhi.h>
#include <assets/loader/ShaderFactory.h>
#include <core/ThreadPool.h>
#include <scene/SceneRenderData.h>
#include <unordered_set>

#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>

namespace caustica
{
    class RootFileSystem;
}

class PTPipelineVariant
{
    // contains all things needed to compile and build the library
    struct ShaderPermutation
    {
        std::filesystem::path                   shaderSrcFileName;
        std::filesystem::path                   shaderOutFileName;

        std::vector<caustica::ShaderMacro> combinedAndSpecializedMacros;   // combined global + variant + per-material-shader-specialized (if any) macros

        std::string                             permutationName;

        caustica::ShaderKey                     cacheKey;
        std::string                             compiledFullPath;
        std::string                             packVfsPath;

        std::string                             usCompileCmdLine;
        std::string                             usCompileError;
        ShaderPermutation *                     usMasterCopy = nullptr;

        nvrhi::ShaderLibraryHandle              shaderLibrary;

        void                                    setPath(const std::filesystem::path & path);
        void                                    fromMaterialPermutation(const std::string & shortUniqueDebugID, const std::vector<caustica::ShaderMacro> & macros, const struct MaterialShaderPermutation & msp);
        void                                    resolveCacheIdentity(class PathTracingShaderCompiler & compiler, std::filesystem::file_time_type lastModifiedSourceCode);
        void                                    compileIfNeeded();
        void                                    resetShaderLibrary();
        void                                    loadShaderLibraryIfNeeded(class PathTracingShaderCompiler & compiler);
    };

private:
    friend class PathTracingShaderCompiler;
    PTPipelineVariant(const std::string & relativeSourcePath, const std::vector<caustica::ShaderMacro> & variantMacros, const std::shared_ptr<PathTracingShaderCompiler> & compiler, const std::string & shortUniqueDebugID, bool raygenOnly);
public:
    ~PTPipelineVariant();

public:

    // TODO: this will invalidate content; make sure to set m_localVerison to -1 and call resetPipeline()
    //void                                    SetMacros(const std::vector<caustica::ShaderMacro> & variantMacros);

    const nvrhi::rt::ShaderTableHandle &    getShaderTable() const { return m_shaderTable; }

private:
    void                                    updateStart(std::filesystem::file_time_type lastModifiedSourceCode);
    void                                    updateFinalize();
    void                                    rebuildShaderTableOnly();
    int64_t                                 getVersion() const { return m_localVersion; }

private:
    void                                    compileIfNeededEnqueue(std::filesystem::file_time_type lastModifiedSourceCode);
    void                                    resetPipeline();

private:

    int64_t                                 m_localVersion = -1;        // if it doesn't match compiler version, we're out of date
    nvrhi::rt::ShaderTableHandle            m_shaderTable;
    nvrhi::rt::PipelineHandle               m_pipeline;
    std::weak_ptr<class PathTracingShaderCompiler>    m_compiler;
    bool                                    m_rayGenOnly;
    bool                                    m_exportAnyHit = false;
    bool                                    m_exportMiss = false;
    std::shared_ptr<class PathTracingShaderCompiler>  m_lockedCompiler;


    std::vector<caustica::ShaderMacro> m_macros;                   // global macros (obtained from PathTracingShaderCompiler::getMacros)
    std::vector<caustica::ShaderMacro> m_combinedMacros;           // per-PTPipelineVariant macros 


    // Raygen and (optional) Miss
    ShaderPermutation                       m_raygen;
    
    // ClosestHit and AnyHit (AnyHit usually baked in and not separate)
    ShaderPermutation                       m_ubershaderMaterial;               // A generic, non-specialized-works-for-all-material (note: this will likely go away in the future)
    std::vector<ShaderPermutation>          m_specializedPerMaterial;           // See MaterialGpuCache's m_shaderPermutationTable

    std::string                             m_shortUniqueDebugID;
};

struct HitGroupInfo
{
    std::shared_ptr<MaterialShaderPermutation> materialShaderPermutation;
    bool        hasAnyHitShader;

    std::string getExportName() const;
    std::string getShaderPermutationName() const;
    int         getShaderPermutationIndex() const;
};

class PathTracingShaderCompiler : public std::enable_shared_from_this<PathTracingShaderCompiler>
{
public:
    PathTracingShaderCompiler(nvrhi::IDevice* device, std::shared_ptr<class MaterialGpuCache> & materialGpuCache, nvrhi::BindingLayoutHandle bindingLayout, nvrhi::BindingLayoutHandle bindlessLayout);
    ~PathTracingShaderCompiler();
    
    void                                update(const caustica::scene::SceneRenderData* sceneData, unsigned int subInstanceCount, const std::function<void(std::vector<caustica::ShaderMacro> & macros)>& globalMacrosGetter, bool forceShaderReload);
    std::shared_ptr<PTPipelineVariant>  createVariant(const std::string & relativeSourcePath, std::vector<caustica::ShaderMacro> variantMacros, const std::string & shortUniqueDebugID, bool rayGenOnly = false );
    void                                releaseVariant(std::shared_ptr<PTPipelineVariant> & variant);
    
    const std::shared_ptr<class MaterialGpuCache> & 
                                        getMaterialGpuCache() const           { return m_materialGpuCache; }

private:
    friend class PTPipelineVariant;
    const std::vector<HitGroupInfo> &   getPerSubInstanceHitGroup() const   { return m_perSubInstanceHitGroup; }
    const std::unordered_map<int, HitGroupInfo> &   
                                        getUniqueHitGroups() const          { return m_uniqueHitGroups; }
    const std::vector<caustica::ShaderMacro> & 
                                        getMacros() const                   { return m_macros; }
    int64_t                             getVersion() const                  { return m_version; }

    nvrhi::BindingLayoutHandle          getBindingLayout() const            { return m_bindingLayout; }
    nvrhi::BindingLayoutHandle          getBindlessLayout() const           { return m_bindlessLayout; }
    nvrhi::DeviceHandle                 getDevice() const                   { return m_device; }

    const std::filesystem::path &       getShaderBinariesPath() const       { return m_compilerConfig.ShaderBinariesPath; }
    const std::filesystem::path &       getShaderCompilerPath() const       { return m_compilerConfig.ShaderCompilerPath; }
    const std::filesystem::path &       getShadersPath() const              { return m_compilerConfig.ShadersPath; }
    const std::filesystem::path &       getShadersPathExternalIncludes1() const { return m_compilerConfig.ShadersPathExternalIncludes1; }
    const std::filesystem::path &       getShadersPathExternalIncludes2() const { return m_compilerConfig.ShadersPathExternalIncludes2; }

    bool                                isVerbose() const                       { return m_verbose; }
    bool                                isNvapiShaderExtensionEnabled() const   { return m_enableNVAPIShaderExtension; }
    bool                                canCompileShaders() const               { return m_compilerConfig.canCompile(); }
    bool                                isLoadOnlyMode() const                  { return !m_compilerConfig.canCompile(); }

    const ShaderCompilerUtils::ShaderCompilerConfig& 
                                        getCompilerConfig() const           { return m_compilerConfig; }

    std::shared_ptr<caustica::RootFileSystem>     
                                        getFs()                             { return m_shadersFS; }
    std::mutex &                        getMutex()                          { return m_mutex; }

    void                                enqueueShaderPermutation(PTPipelineVariant::ShaderPermutation* perm);

    bool                                registerShortUniqueDebugID(const std::string & id);
    void                                unregisterShortUniqueDebugID(const std::string & id);


private:
    nvrhi::DeviceHandle                             m_device;
    std::shared_ptr<caustica::RootFileSystem>     m_shadersFS;
    std::shared_ptr<class MaterialGpuCache> &         m_materialGpuCache;
    nvrhi::BindingLayoutHandle                      m_bindingLayout;
    nvrhi::BindingLayoutHandle                      m_bindlessLayout;

    ShaderCompilerUtils::ShaderCompilerConfig       m_compilerConfig;

    std::vector<caustica::ShaderMacro>         m_macros;

    std::vector<std::shared_ptr<PTPipelineVariant>> m_variants;

    std::vector<PTPipelineVariant::ShaderPermutation*> m_parallelCompileListAll;
    std::unordered_map<std::string, PTPipelineVariant::ShaderPermutation*> m_parallelCompileListUnique; // one big list of enqueued files to compile, tracked by output name to avoid duplicates as multiple PTPipelineVariant-s can request same outputs

    std::unordered_set<std::string>                 m_shortUniqueDebugIDs;

    // Hit groups shared between pipeline variants.
    std::vector<HitGroupInfo>                       m_perSubInstanceHitGroup;
    std::unordered_map<int, HitGroupInfo>           m_uniqueHitGroups;

    std::optional<std::filesystem::file_time_type>  m_lastUpdatedSourceTimestamp;

    bool                                            m_verbose = false;
    bool                                            m_enableNVAPIShaderExtension = false;

    caustica::ThreadPool                       m_threadPool;

    int64_t                                         m_version = -1;
    // Once RT pipelines exist, runtime imports only remap hit-group table entries onto
    // this frozen unique set. Growing unique exports forces createRayTracingPipeline and
    // can stall the render thread for a very long time (and hang window close).
    bool                                            m_uniqueHitGroupsFrozen = false;
    uint64_t                                        m_materialStateRevision = 0;
    uint64_t                                        m_resourceBindingRevision = 0;

    std::mutex                                      m_mutex;    // for synchronizing work by variants

    // Auto-reload via polling
    float                                           m_autoReloadPollIntervalSeconds = 0.5f;
    std::optional<std::filesystem::file_time_type>  m_cachedSourceTimestamp;
};
