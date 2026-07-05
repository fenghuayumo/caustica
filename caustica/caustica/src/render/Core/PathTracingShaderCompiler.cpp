#include <render/Core/PathTracingShaderCompiler.h>
#include <assets/loader/PathTracingShaderBuild.h>
#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>
#include <assets/loader/ShaderPackFileSystem.h>

#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>

#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderData.h>
#include <shaders/PathTracer/PathTracerShared.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/system_utils.h>

using namespace caustica;

#define BAKER_ENABLE_MULTITHREADED_COMPILE_SHADER 1
#define BAKER_ENABLE_MULTITHREADED_COMPILE_PSO 0

// this will add pipeline variant's m_shortUniqueDebugID to raygen/miss and m_shortUniqueDebugID + material permutation name to all closesthit and anyhit shaders
// this is very useful for debugging
#define PIPELINE_BAKER_ENABLE_VERBOSE_FUNCTION_NAMING 1

// this is for testing
#define PIPELINE_BAKER_REVERT_TO_UBERSHADER 0

#define PIPELINE_BAKER_EMBED_PDBS 0 // otherwise export as .pdb-s

#define PIPELINE_BAKER_USE_OPTIMIZATIONS 1

using namespace caustica::math;
using namespace caustica;

static const std::string c_PTShaderBinariesRoot = "ShaderDynamic/Bin";
static const std::string c_PTShaderPackMount = "/" + c_PTShaderBinariesRoot;

#if PIPELINE_BAKER_USE_OPTIMIZATIONS
static constexpr bool c_PTUseOptimizations = true;
#else
static constexpr bool c_PTUseOptimizations = false;
#endif

#if PIPELINE_BAKER_EMBED_PDBS
static constexpr bool c_PTEmbedPdbs = true;
#else
static constexpr bool c_PTEmbedPdbs = false;
#endif

void PTPipelineVariant::ShaderPermutation::SetPath(const std::filesystem::path & path)
{
    ShaderSrcFileName = path;
    assert(ShaderSrcFileName.extension().string() == ".hlsl");

    ShaderOutFileName = ShaderSrcFileName;
    ShaderOutFileName.replace_extension();
}

void PTPipelineVariant::ShaderPermutation::FromMaterialPermutation(const std::string & shortUniqueDebugID, const std::vector<caustica::ShaderMacro> & macros, const struct MaterialShaderPermutation & msp)
{
    SetPath( msp.ShaderFilePath );
    CombinedAndSpecializedMacros = macros;
    for (auto& macro : msp.Macros)
        CombinedAndSpecializedMacros.push_back(macro);
    PermutationName = shortUniqueDebugID + "_" + msp.StableShaderName;
    CombinedAndSpecializedMacros.push_back( {"CAUSTICA_MATERIAL_PERMUTATION_NAME", PermutationName } );  // used to rename ClosestHit (and, if any, AnyHit) to something readable in profilers and etc

    CombinedAndSpecializedMacros.push_back( {"CAUSTICA_SHADER_ID", std::to_string(msp.StableShaderID) } );  // used for stable shader debug coloring
}

void PTPipelineVariant::ShaderPermutation::CompileIfNeeded()
{
    if (US_compileCmdLine == "")
        return;

    caustica::info("Compiling shader '%s' file '%s'...", PermutationName.c_str(), CompiledFullPath.c_str() );

    auto [resNum, resString, resErrorString] = SystemShell(US_compileCmdLine, false);

    US_compileError = "";

    if (resErrorString != "")
        US_compileError = StringFormat("ERROR compiling shader, command \n   %s\n result: \n   %s", US_compileCmdLine.c_str(), resErrorString.c_str());

    if (US_compileError != "")
        caustica::warning(US_compileError.c_str());
}

void PTPipelineVariant::ShaderPermutation::ResetShaderLibrary()
{
    ShaderLibrary = nullptr;
}

void PTPipelineVariant::ShaderPermutation::LoadShaderLibraryIfNeeded(PathTracingShaderCompiler & compiler)
{
    if (US_compileError != "" || ShaderLibrary != nullptr)
        return;

    std::shared_ptr<caustica::IBlob> data = compiler.GetFS()->readFile(PackVfsPath.c_str());

    if (data)
        ShaderLibrary = compiler.GetDevice()->createShaderLibrary(data->data(), data->size());

    if (!ShaderLibrary)
    {
        US_compileError = StringFormat(
            "Failed to load shader library '%s' (pack path '%s').",
            CompiledFullPath.c_str(),
            PackVfsPath.c_str());
    }
}

void PTPipelineVariant::ShaderPermutation::ResolveCacheIdentity(
    PathTracingShaderCompiler& compiler,
    std::filesystem::file_time_type lastModifiedSourceCode)
{
    US_compileError = "";
    US_compileCmdLine = "";

    const auto srcFullPath = std::filesystem::absolute(compiler.GetShadersPath() / ShaderSrcFileName);
    const PathTracingShaderBuildInput buildInput{
        .logicalSourcePath = ShaderSrcFileName,
        .absoluteSourcePath = srcFullPath,
        .macros = CombinedAndSpecializedMacros,
        .useOptimizations = c_PTUseOptimizations,
        .embedPdbs = c_PTEmbedPdbs,
    };

    const PathTracingShaderBuildResult buildResult =
        BuildPathTracingLibraryShader(compiler.GetCompilerConfig(), buildInput);

    const std::string previousHashHex = CacheKey.cacheHashHex;
    CacheKey = buildResult.key;

    const bool cacheIdentityChanged = previousHashHex != CacheKey.cacheHashHex;
    if (cacheIdentityChanged)
        ResetShaderLibrary();

    CompiledFullPath = CacheKey.cacheFilePath(compiler.GetShaderBinariesPath()).string();
    PackVfsPath = CacheKey.packVfsPath(c_PTShaderPackMount);

    const bool compiledBlobAvailable = compiler.GetFS()->fileExists(PackVfsPath);
    const bool diskBlobUpToDate = ShaderCompilerUtils::IsCompiledShaderUpToDate(
        CompiledFullPath,
        lastModifiedSourceCode);

    if (compiledBlobAvailable && (!compiler.CanCompileShaders() || diskBlobUpToDate))
    {
        if (compiler.IsVerbose())
        {
            caustica::info(
                "Using cached shader '%s' (%s)...",
                PermutationName.c_str(),
                CacheKey.cacheFileNameNoExt().c_str());
        }
        return;
    }

    if (!compiler.CanCompileShaders())
    {
        US_compileError = StringFormat(
            "Missing precompiled shader '%s' and runtime shader compilation is disabled.",
            CompiledFullPath.c_str());
        caustica::error("%s", US_compileError.c_str());
        return;
    }

    EnsureDirectoryExists(std::filesystem::path(CompiledFullPath).parent_path());

    const std::filesystem::path pdbPath =
        compiler.GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN || c_PTEmbedPdbs
        ? std::filesystem::path{}
        : std::filesystem::path(CompiledFullPath).replace_extension(".pdb");

    US_compileCmdLine = MakePathTracingShaderCompileCommand(
        compiler.GetCompilerConfig(),
        buildResult,
        CompiledFullPath,
        pdbPath);

    if (compiler.IsVerbose())
        caustica::info("Enqueuing shader variant of '%s'...", srcFullPath.string().c_str());
}

PTPipelineVariant::PTPipelineVariant(const std::string & relativeSourcePath, const std::vector<caustica::ShaderMacro> & variantMacros, const std::shared_ptr<PathTracingShaderCompiler>& compiler, const std::string & shortUniqueDebugID, bool raygenOnly)
    : m_macros(variantMacros), m_compiler(compiler), m_rayGenOnly(raygenOnly)
{
    m_raygen.SetPath(relativeSourcePath);

    // short unique ID distinguishes exports between pipeline variants and must be safe for HLSL entry point names
#if PIPELINE_BAKER_ENABLE_VERBOSE_FUNCTION_NAMING
    m_shortUniqueDebugID = StripNonAsciiAlnum(shortUniqueDebugID);
    assert( shortUniqueDebugID == m_shortUniqueDebugID ); // short unique debug ID must not contain any of the forbidden characters or bad things will happen, very bad
    if (m_shortUniqueDebugID.size()>6)
    {
        assert( false ); // too big
        m_shortUniqueDebugID = m_shortUniqueDebugID.substr(0, 6);
    }
    if (!compiler->RegisterShortUniqueDebugID(m_shortUniqueDebugID))
    {
        assert( false ); // not unique!
    }
#else
    m_shortUniqueDebugID = "UNK";
#endif
}

PTPipelineVariant::~PTPipelineVariant()
{
    std::shared_ptr<PathTracingShaderCompiler> & baker = m_lockedCompiler; 
    if (baker)
        baker->UnregisterShortUniqueDebugID(m_shortUniqueDebugID);
}

void PTPipelineVariant::ResetPipeline()
{
    m_shaderTable = nullptr;
    m_pipeline = nullptr;
    m_localVersion = -1;
}

void PTPipelineVariant::CompileIfNeeded_Enqueue(std::filesystem::file_time_type lastModifiedSourceCode)
{
    std::shared_ptr<PathTracingShaderCompiler> & baker = m_lockedCompiler; assert( baker != nullptr );

    // no further specializations for these two
    m_raygen.CombinedAndSpecializedMacros = m_combinedMacros;
    m_raygen.PermutationName = m_shortUniqueDebugID + "_raygen";
    m_raygen.CombinedAndSpecializedMacros.push_back( {"CAUSTICA_PIPELINE_PERMUTATION_NAME", m_shortUniqueDebugID } );  // used to rename ClosestHit (and, if any, AnyHit) to something readable in profilers and etc
    
    m_ubershaderMaterial.FromMaterialPermutation( m_shortUniqueDebugID, m_combinedMacros, *baker->GetMaterialGpuCache()->GetUbershader() );

    m_specializedPerMaterial.clear();
    const auto & sourceTable = baker->GetMaterialGpuCache()->GetShaderPermutationTable();
    m_specializedPerMaterial.resize(sourceTable.size());
    for( int i = 0; i < sourceTable.size(); i++ )
        m_specializedPerMaterial[i].FromMaterialPermutation( m_shortUniqueDebugID, m_combinedMacros, *(sourceTable[i]) );

    std::vector<ShaderPermutation*>         currentList;

    currentList.clear();
    currentList.push_back(&m_raygen);
    currentList.push_back(&m_ubershaderMaterial);

    if (!m_rayGenOnly)
        for( int i = 0; i < m_specializedPerMaterial.size(); i++ )
            currentList.push_back( &(m_specializedPerMaterial[i]) );

    bool resetPipelineNeeded = false;
    for( ShaderPermutation * permutation : currentList )
    {
        const std::string previousHashHex = permutation->CacheKey.cacheHashHex;
        permutation->ResolveCacheIdentity(*baker, lastModifiedSourceCode);
        if (permutation->CacheKey.cacheHashHex != previousHashHex || !permutation->US_compileCmdLine.empty())
            resetPipelineNeeded = true;
        baker->EnqueueShaderPermutation(permutation);
    }

    if (resetPipelineNeeded)
        ResetPipeline();
}

//void PTPipelineVariant::CompileIfNeeded_Execute()
//{
//    std::shared_ptr<PathTracingShaderCompiler>& baker = m_lockedCompiler; assert(baker != nullptr);
//}

void PTPipelineVariant::UpdateStart(std::filesystem::file_time_type lastModifiedSourceCode)
{
    assert( m_lockedCompiler == nullptr );

    std::shared_ptr<PathTracingShaderCompiler> & compiler = m_lockedCompiler = m_compiler.lock();
    if (compiler == nullptr)
    {
        assert( false );
        return;
    }

    m_combinedMacros.clear(); m_combinedMacros.reserve( m_macros.size() + compiler->GetMacros().size() );
    for (auto& macro : m_macros)
        m_combinedMacros.push_back(macro);
    for (auto& macro : compiler->GetMacros())
        m_combinedMacros.push_back(macro);
    
    bool foundExportAnyHitDependency = false;
    for (auto& macro : m_combinedMacros)
    {
        if (macro.name == "USE_NVAPI_HIT_OBJECT_EXTENSION")
        {
            assert(!foundExportAnyHitDependency); // can and must have only 1
            if (macro.definition == "1")
                m_exportAnyHit = m_exportMiss = false;
            else
                m_exportAnyHit = m_exportMiss = true;
            foundExportAnyHitDependency = true;
        }
    }
    m_exportMiss = true; // it looks like not exporting a miss is a bug or considered a bug - need to verify
    assert(foundExportAnyHitDependency); // any changes in the way USE_NVAPI_HIT_OBJECT_EXTENSION is used?

    CompileIfNeeded_Enqueue(lastModifiedSourceCode);
}

void PTPipelineVariant::UpdateFinalize()
{
    std::shared_ptr<PathTracingShaderCompiler> & baker = m_lockedCompiler; assert( baker != nullptr );

    // if no changes, no need to re-create pipeline
    if (m_pipeline == nullptr )
    {
        if (m_raygen.ShaderLibrary == nullptr)
        {
            assert(false);
            return;
        }

        std::string rayGenName  = ("RayGen_"+m_shortUniqueDebugID);
        std::string missName    = ("Miss_"+m_shortUniqueDebugID);

        nvrhi::rt::PipelineDesc pipelineDesc;
        pipelineDesc.globalBindingLayouts = { baker->GetBindingLayout(), baker->GetBindlessLayout() };
        pipelineDesc.shaders.push_back({ "", m_raygen.ShaderLibrary->getShader(rayGenName.c_str(), nvrhi::ShaderType::RayGeneration), nullptr });
        pipelineDesc.shaders.push_back({ "", m_raygen.ShaderLibrary->getShader(missName.c_str(), nvrhi::ShaderType::Miss), nullptr });
        pipelineDesc.allowOpacityMicromaps = false;
        for (const auto& macro : m_combinedMacros)
        {
            if (macro.name == "CAUSTICA_ENABLE_OPACITY_MICROMAPS")
            {
                pipelineDesc.allowOpacityMicromaps = macro.definition == "1";
                break;
            }
        }
        
        if (!m_rayGenOnly)
        for (auto& [_, hitGroupInfo] : baker->GetUniqueHitGroups())
        {
            std::string shaderPermutationName = hitGroupInfo.GetShaderPermutationName();
            std::string closestHit = "ClosestHit_" + m_shortUniqueDebugID + "_" + shaderPermutationName;
            std::string anyHit = (m_exportAnyHit && hitGroupInfo.HasAnyHitShader)?("AnyHit_" + m_shortUniqueDebugID + "_" + shaderPermutationName):("");

#if PIPELINE_BAKER_REVERT_TO_UBERSHADER
            const ShaderPermutation & permutation = m_ubershaderMaterial;
#else
            const ShaderPermutation & permutation = m_specializedPerMaterial[hitGroupInfo.GetShaderPermutationIndex()];
#endif
            if (permutation.ShaderLibrary == nullptr)
            {
                assert(false);
                return;
            }
            pipelineDesc.hitGroups.push_back(
                {
                    .exportName = hitGroupInfo.GetExportName(),
                    .closestHitShader = permutation.ShaderLibrary->getShader(closestHit.c_str(), nvrhi::ShaderType::ClosestHit),
                    .anyHitShader = (anyHit != "") ? (permutation.ShaderLibrary->getShader(anyHit.c_str(), nvrhi::ShaderType::AnyHit)) : (nullptr),
                    .intersectionShader = nullptr,
                    .bindingLayout = nullptr,
                    .isProceduralPrimitive = false
                }
            );
        }

        pipelineDesc.maxPayloadSize = PATH_TRACER_MAX_PAYLOAD_SIZE;
        pipelineDesc.maxRecursionDepth = 1; // 1 is enough if using inline visibility rays
        
        // NV HLSL extensions - DX12 only - we should probably expose some form of GetNvapiIsInitialized instead
        bool usesNvapiHitObjectExtension = false;
        for (const auto& macro : m_combinedMacros)
        {
            if (macro.name == "USE_NVAPI_HIT_OBJECT_EXTENSION")
            {
                usesNvapiHitObjectExtension = macro.definition == "1";
                break;
            }
        }
        if (usesNvapiHitObjectExtension && baker->IsNVAPIShaderExtensionEnabled())
            pipelineDesc.hlslExtensionsUAV = NV_SHADER_EXTN_SLOT_NUM;

        m_pipeline = baker->GetDevice()->createRayTracingPipeline(pipelineDesc);

        if (!m_pipeline)
            { assert( false ); return; }

        m_shaderTable = m_pipeline->createShaderTable();

        if (!m_shaderTable)
            { assert( false ); return; }

        m_shaderTable->setRayGenerationShader(rayGenName.c_str());

        if (!m_rayGenOnly)
        {
            auto& perSubInstanceHitGroup = baker->GetPerSubInstanceHitGroup();
            for (int i = 0; i < perSubInstanceHitGroup.size(); i++)
                m_shaderTable->addHitGroup(perSubInstanceHitGroup[i].GetExportName().c_str());
        }

        if (m_exportMiss)
            m_shaderTable->addMissShader(missName.c_str());

        m_localVersion = baker->GetVersion();

        assert(m_pipeline != nullptr && m_shaderTable != nullptr);
    }

    m_lockedCompiler = nullptr;
}


PathTracingShaderCompiler::PathTracingShaderCompiler(nvrhi::IDevice* device, std::shared_ptr<MaterialGpuCache>& materialGpuCache, nvrhi::BindingLayoutHandle bindingLayout, nvrhi::BindingLayoutHandle bindlessLayout)
    : m_device(device)
    , m_materialGpuCache(materialGpuCache)
    , m_bindingLayout(bindingLayout)
    , m_bindlessLayout(bindlessLayout)
    , m_enableNVAPIShaderExtension(device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV))
{
    if (!m_compilerConfig.Initialize(device, c_PTShaderBinariesRoot))
        caustica::fatal("Failed to initialize shader compiler configuration");

    m_shadersFS = std::make_shared<caustica::RootFileSystem>();
    const char* shaderTypeName = caustica::GetShaderTypeName(device->getGraphicsAPI());
    const std::filesystem::path shaderPackPath = GetRuntimeDirectory() / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, c_PTShaderBinariesRoot);
    const bool shaderPackHasDynamicBins = shaderPackFS->hasDynamicBinLayout(m_compilerConfig.ShaderBinariesPath);
    if (shaderPackFS->isOpen() && !shaderPackHasDynamicBins)
    {
        caustica::warning(
            "Shader pack '%s' does not include ShaderDynamic bins; falling back to '%s'.",
            shaderPackPath.string().c_str(),
            m_compilerConfig.ShaderBinariesPath.string().c_str());
    }

    if (shaderPackHasDynamicBins)
    {
        m_shadersFS->mount("/" + c_PTShaderBinariesRoot, shaderPackFS);
        m_compilerConfig.RuntimeCompilationAvailable = false;
        caustica::info(
            "PathTracingShaderCompiler: load-only mode using shader pack '%s'.",
            shaderPackPath.string().c_str());
    }
    else
    {
        m_shadersFS->mount("/" + c_PTShaderBinariesRoot, m_compilerConfig.ShaderBinariesPath);
        if (m_compilerConfig.CanCompile())
        {
            caustica::info("PathTracingShaderCompiler: dev mode — runtime DXC compilation enabled.");
        }
        else
        {
            caustica::info(
                "PathTracingShaderCompiler: load-only mode using '%s' (no shader pack or DXC).",
                m_compilerConfig.ShaderBinariesPath.string().c_str());
        }
    }
}

PathTracingShaderCompiler::~PathTracingShaderCompiler()
{
}

std::string HitGroupInfo::GetExportName() const { return "HitGroup_" + std::to_string(MaterialShaderPermutation->IndexInTable); }


#if PIPELINE_BAKER_ENABLE_VERBOSE_FUNCTION_NAMING
std::string HitGroupInfo::GetShaderPermutationName() const { return MaterialShaderPermutation->StableShaderName; }
#else
std::string HitGroupInfo::GetShaderPermutationName() const { return MaterialShaderPermutation->StableShaderName; }
#endif

int HitGroupInfo::GetShaderPermutationIndex() const { return MaterialShaderPermutation->IndexInTable; }

HitGroupInfo ComputeSubInstanceHitGroupInfo(const MaterialGpuCache & baker, const PTMaterial& material)
{
    HitGroupInfo info;

#if PIPELINE_BAKER_REVERT_TO_UBERSHADER
    info.MaterialShaderPermutation = baker.GetUbershader();
#else
    info.MaterialShaderPermutation = material.BakedShaderPermutation;
#endif

    info.HasAnyHitShader = material.HasAlphaTest();
    
    return info;
}

HitGroupInfo ComputeDefaultSubInstanceHitGroupInfo(const MaterialGpuCache& baker)
{
    HitGroupInfo info;
    info.MaterialShaderPermutation = baker.GetUbershader();
    info.HasAnyHitShader = false;
    return info;
}

static bool macrosEqual(caustica::ShaderMacro& a, caustica::ShaderMacro& b)
{
    return a.name == b.name && a.definition == b.definition;
}

void PathTracingShaderCompiler::EnqueueShaderPermutation(PTPipelineVariant::ShaderPermutation* perm)
{
    if (perm->US_compileCmdLine != "")
    {
        auto [it, inserted] = m_parallelCompileListUnique.try_emplace(perm->CacheKey.cacheFileNameNoExt(), perm);
        if (!inserted)
            perm->US_masterCopy = it->second;
    }
    m_parallelCompileListAll.push_back(perm);
}

void PathTracingShaderCompiler::Update(const std::shared_ptr<caustica::Scene>& scene, unsigned int subInstanceCount, const std::function<void(std::vector<caustica::ShaderMacro>& macros)>& globalMacrosGetter, bool forceShaderReload)
{
    // Auto-reload: poll for source file changes
    if (m_compilerConfig.CanCompile() && !forceShaderReload && !m_variants.empty())
    {
        static auto lastPollTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastPollTime).count();
        
        if (elapsed >= m_autoReloadPollIntervalSeconds)
        {
            lastPollTime = now;
            auto currentTimestamp = GetLatestModifiedTimeDirectoryRecursive(m_compilerConfig.ShadersPath);
            
            if (currentTimestamp.has_value())
            {
                if (!m_cachedSourceTimestamp.has_value())
                {
                    // First poll - just cache the timestamp
                    m_cachedSourceTimestamp = currentTimestamp;
                }
                else if (*currentTimestamp != *m_cachedSourceTimestamp)
                {
                    // Source files changed - trigger reload
                    m_cachedSourceTimestamp = currentTimestamp;
                    forceShaderReload = true;
                    caustica::info("RT shader source changes detected - triggering hot reload...");
                }
            }
        }
    }

    bool needsUpdate = m_perSubInstanceHitGroup.size() != subInstanceCount;
    needsUpdate |= subInstanceCount > 0 && m_uniqueHitGroups.empty();
    for (const std::shared_ptr<PTPipelineVariant>& variant : m_variants)
        needsUpdate |= variant->m_pipeline == nullptr;

    std::vector<caustica::ShaderMacro> newMacros;
    globalMacrosGetter(newMacros);
    if (newMacros.size() != m_macros.size() || !std::equal(newMacros.begin(), newMacros.end(), m_macros.begin(), macrosEqual))
    {
        needsUpdate = true;
        m_macros = newMacros;
    }

    needsUpdate |= forceShaderReload;

    // no need to update these if already set up
    if (needsUpdate) // m_uniqueHitGroups.size() == 0)
    {
        // Note: these map 1-1 to m_subInstanceData, and are used to (see '->addHitGroup' below) build 1-1 mapped hit groups 
        m_perSubInstanceHitGroup.clear();
        m_perSubInstanceHitGroup.assign(subInstanceCount, ComputeDefaultSubInstanceHitGroupInfo(*GetMaterialGpuCache()));
        for (const scene::MeshInstanceRenderProxy& proxy : scene->GetRenderData().meshInstances)
        {
            if (!proxy.mesh || proxy.geometryInstanceIndex < 0)
                continue;

            const size_t firstSubInstanceIndex = static_cast<size_t>(proxy.geometryInstanceIndex);
            for (size_t gi = 0; gi < proxy.mesh->geometries.size(); gi++)
            {
                const size_t subInstanceIndex = firstSubInstanceIndex + gi;
                if (subInstanceIndex >= m_perSubInstanceHitGroup.size() || !proxy.mesh->geometries[gi])
                    continue;

                std::shared_ptr<PTMaterial> materialPT = PTMaterial::SafeCast(proxy.mesh->geometries[gi]->material);
                if (!materialPT)
                    continue;

                m_perSubInstanceHitGroup[subInstanceIndex] = ComputeSubInstanceHitGroupInfo(*GetMaterialGpuCache(), *materialPT);
            }
        }
        assert(m_perSubInstanceHitGroup.size() == subInstanceCount);

        // Prime the instances to make sure we only include the necessary CHS variants in the PSO. Many (sub)instances can map to same materials.
        m_uniqueHitGroups.clear();
        for (int i = 0; i < m_perSubInstanceHitGroup.size(); i++)
            m_uniqueHitGroups[m_perSubInstanceHitGroup[i].GetShaderPermutationIndex()] = m_perSubInstanceHitGroup[i];
        needsUpdate = true;
    }

    if (needsUpdate)
    {
        m_version++;
    }

    if (needsUpdate)
    {
        if (m_compilerConfig.CanCompile())
        {
            // we need the output folder
            EnsureDirectoryExists(m_compilerConfig.ShaderBinariesPath);
        }

        std::optional<std::filesystem::file_time_type> a = m_compilerConfig.CanCompile()
            ? GetLatestModifiedTimeDirectoryRecursive(m_compilerConfig.ShadersPath)
            : std::optional<std::filesystem::file_time_type>(std::filesystem::file_time_type::min());
        // let's not track externals for perf reasons but here's the code in case it's needed
        //std::optional<std::filesystem::file_time_type> b = GetLatestModifiedTimeRecursive(m_compilerConfig.ShadersPathExternalIncludes1);
        //std::optional<std::filesystem::file_time_type> c = GetLatestModifiedTimeRecursive(m_compilerConfig.ShadersPathExternalIncludes2);
        m_lastUpdatedSourceTimestamp = a;
    }

    if (!m_lastUpdatedSourceTimestamp.has_value())
    {
        caustica::error("There is something wrong with the shader source path or logic - unable to load or dynamically compile shaders");
        return;
    }

    if (!needsUpdate)
        return;

    do // in case of compile errors allow user to modify and attempt recompile
    {
        assert( m_parallelCompileListAll.empty() && m_parallelCompileListUnique.empty() );

        std::vector<std::shared_ptr<PTPipelineVariant>> updateQueue;
        for (int i = 0; i < int(m_variants.size()); i++)
        {
            const std::shared_ptr<PTPipelineVariant>& variant = m_variants[i];
            if (variant.use_count() == 1)
                assert(false); // dangling Variant - forgotten a call to ReleaseVariant?
            if (variant->GetVersion() != m_version)
            {
                updateQueue.push_back(variant);
                variant->ResetPipeline();
                variant->UpdateStart(*m_lastUpdatedSourceTimestamp);
            }
        }

        std::atomic_int progressCounterCompleted;
        int progressTotal;

        ProgressBar progressCompilingShaders;
        progressCounterCompleted = 0;
        progressTotal = (int)m_parallelCompileListUnique.size();
        if (m_parallelCompileListUnique.size() > 0)
            progressCompilingShaders.Start(StringFormat("Compiling shaders (%d)...", progressTotal).c_str());
        else if (IsLoadOnlyMode() && !updateQueue.empty())
            caustica::info("PathTracingShaderCompiler: loading precompiled RT shader libraries...");

        for (auto it : m_parallelCompileListUnique)
        {
            PTPipelineVariant::ShaderPermutation* permutation = it.second;
            if (permutation->US_compileCmdLine == "")
            {
                assert(false); continue;
            } // not sure why this would happen

#if BAKER_ENABLE_MULTITHREADED_COMPILE_SHADER
            m_threadPool.AddTask([ this, permutation, &progressCompilingShaders, &progressCounterCompleted, progressTotal ]() {
#endif
                permutation->CompileIfNeeded();
                int completed = progressCounterCompleted.fetch_add(1) + 1;
                progressCompilingShaders.Set(0 + 100 * completed / progressTotal);

#if BAKER_ENABLE_MULTITHREADED_COMPILE_SHADER
            });
#endif
        }

// wait for all to complete
#if BAKER_ENABLE_MULTITHREADED_COMPILE_SHADER
        m_threadPool.WaitForTasks();
#endif
 
        std::string firstError = "";

        // we've got to 
        for (PTPipelineVariant::ShaderPermutation* permutation : m_parallelCompileListAll)
        {
            // if error, break & skip
            if (permutation->US_compileError != "")
            {
                firstError = permutation->US_compileError;
                break;
            }

            permutation->LoadShaderLibraryIfNeeded(*this);
            // if error, break & skip
            if (permutation->ShaderLibrary == nullptr)
            {
                firstError = permutation->US_compileError;
                break;
            }
        }

        progressCompilingShaders.Set(100);
        m_parallelCompileListAll.clear();
        m_parallelCompileListUnique.clear();

        ProgressBar progressCompilingPSOs;
        progressCounterCompleted = 0;
        progressTotal = (int)updateQueue.size();
        if (progressTotal > 0)
            progressCompilingPSOs.Start( StringFormat("Compiling PSOs (%d)...", progressTotal).c_str() );

        if (!updateQueue.empty() && firstError == "")
        {
            int updateQueueSize = (int)updateQueue.size();

            for (const std::shared_ptr<PTPipelineVariant>& variant : updateQueue)
            {
    #if BAKER_ENABLE_MULTITHREADED_COMPILE_PSO
                m_threadPool.AddTask([this, variant, &progressCompilingPSOs, &progressCounterCompleted, progressTotal ](){
    #endif
                variant->UpdateFinalize();
                int completed = progressCounterCompleted.fetch_add(1)+1;
                progressCompilingPSOs.Set(0 + 99 * completed / progressTotal);
    #if BAKER_ENABLE_MULTITHREADED_COMPILE_PSO
                });
    #endif
            }
    #if BAKER_ENABLE_MULTITHREADED_COMPILE_PSO
            m_threadPool.WaitForTasks();
    #endif

            progressCompilingPSOs.Set(100);
        }


        if (firstError!="")
        {
            caustica::error("%s", firstError.c_str());
            bool retry = false;
#if _WIN32
            if (!HelpersIsNonInteractive())
            {
                int result = MessageBoxA((HWND)HelpersGetActiveWindow(), firstError.c_str(),
                    "Shader compile error", MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TASKMODAL);
                retry = result != IDCANCEL;
            }
#endif
            if (!retry)
                break;

            for (const std::shared_ptr<PTPipelineVariant>& variant : updateQueue)
                variant->m_lockedCompiler = nullptr;
        }
        else
        {
            break;
        }

    } while (true);
}

void PathTracingShaderCompiler::ReleaseVariant(std::shared_ptr<PTPipelineVariant>& variant)
{
    if (variant == nullptr)
        return;

    for (int i = int(m_variants.size()) - 1; i >= 0; i--)
    {
        if (m_variants[i] == variant)
        {
            m_variants.erase(m_variants.begin() + i);
            variant = nullptr;
            return;
        }
    }
    assert(false);
}

std::shared_ptr<PTPipelineVariant> PathTracingShaderCompiler::CreateVariant(const std::string & relativeSourcePath, std::vector<caustica::ShaderMacro> variantMacros, const std::string & shortUniqueDebugID, bool rayGenOnly)
{
    std::shared_ptr<PTPipelineVariant> variant = std::shared_ptr<PTPipelineVariant>(new PTPipelineVariant(relativeSourcePath, variantMacros, this->shared_from_this(), shortUniqueDebugID, rayGenOnly));
    m_variants.push_back(variant);
    return variant;
}

bool PathTracingShaderCompiler::RegisterShortUniqueDebugID(const std::string& id)
{
    auto [it, inserted] = m_shortUniqueDebugIDs.insert(id);

    return inserted;
}

void PathTracingShaderCompiler::UnregisterShortUniqueDebugID(const std::string& id)
{
    int count = m_shortUniqueDebugIDs.erase(id);
    assert( count > 0 );
}
