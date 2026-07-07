#include <render/core/PathTracingShaderCompiler.h>
#include <assets/loader/PathTracingShaderBuild.h>
#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>
#include <assets/loader/ShaderPackFileSystem.h>

#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>

#include <render/passes/lighting/MaterialGpuCache.h>
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

void PTPipelineVariant::ShaderPermutation::setPath(const std::filesystem::path & path)
{
    shaderSrcFileName = path;
    assert(shaderSrcFileName.extension().string() == ".hlsl");

    shaderOutFileName = shaderSrcFileName;
    shaderOutFileName.replace_extension();
}

void PTPipelineVariant::ShaderPermutation::fromMaterialPermutation(const std::string & shortUniqueDebugID, const std::vector<caustica::ShaderMacro> & macros, const struct MaterialShaderPermutation & msp)
{
    setPath( msp.shaderFilePath );
    combinedAndSpecializedMacros = macros;
    for (auto& macro : msp.macros)
        combinedAndSpecializedMacros.push_back(macro);
    permutationName = shortUniqueDebugID + "_" + msp.stableShaderName;
    combinedAndSpecializedMacros.push_back( {"CAUSTICA_MATERIAL_PERMUTATION_NAME", permutationName } );  // used to rename ClosestHit (and, if any, AnyHit) to something readable in profilers and etc

    combinedAndSpecializedMacros.push_back( {"CAUSTICA_SHADER_ID", std::to_string(msp.stableShaderId) } );  // used for stable shader debug coloring
}

void PTPipelineVariant::ShaderPermutation::compileIfNeeded()
{
    if (usCompileCmdLine == "")
        return;

    caustica::info("Compiling shader '%s' file '%s'...", permutationName.c_str(), compiledFullPath.c_str() );

    auto [resNum, resString, resErrorString] = SystemShell(usCompileCmdLine, false);

    usCompileError = "";

    if (resErrorString != "")
        usCompileError = StringFormat("ERROR compiling shader, command \n   %s\n result: \n   %s", usCompileCmdLine.c_str(), resErrorString.c_str());

    if (usCompileError != "")
        caustica::warning(usCompileError.c_str());
}

void PTPipelineVariant::ShaderPermutation::resetShaderLibrary()
{
    shaderLibrary = nullptr;
}

void PTPipelineVariant::ShaderPermutation::loadShaderLibraryIfNeeded(PathTracingShaderCompiler & compiler)
{
    if (usCompileError != "" || shaderLibrary != nullptr)
        return;

    std::shared_ptr<caustica::IBlob> data = compiler.getFs()->readFile(packVfsPath.c_str());

    if (data)
        shaderLibrary = compiler.getDevice()->createShaderLibrary(data->data(), data->size());

    if (!shaderLibrary)
    {
        usCompileError = StringFormat(
            "Failed to load shader library '%s' (pack path '%s').",
            compiledFullPath.c_str(),
            packVfsPath.c_str());
    }
}

void PTPipelineVariant::ShaderPermutation::resolveCacheIdentity(
    PathTracingShaderCompiler& compiler,
    std::filesystem::file_time_type lastModifiedSourceCode)
{
    usCompileError = "";
    usCompileCmdLine = "";

    const auto srcFullPath = std::filesystem::absolute(compiler.getShadersPath() / shaderSrcFileName);
    const PathTracingShaderBuildInput buildInput{
        .logicalSourcePath = shaderSrcFileName,
        .absoluteSourcePath = srcFullPath,
        .macros = combinedAndSpecializedMacros,
        .useOptimizations = c_PTUseOptimizations,
        .embedPdbs = c_PTEmbedPdbs,
    };

    const PathTracingShaderBuildResult buildResult =
        buildPathTracingLibraryShader(compiler.getCompilerConfig(), buildInput);

    const std::string previousHashHex = cacheKey.cacheHashHex;
    cacheKey = buildResult.key;

    const bool cacheIdentityChanged = previousHashHex != cacheKey.cacheHashHex;
    if (cacheIdentityChanged)
        resetShaderLibrary();

    compiledFullPath = cacheKey.cacheFilePath(compiler.getShaderBinariesPath()).string();
    packVfsPath = cacheKey.packVfsPath(c_PTShaderPackMount);

    const bool compiledBlobAvailable = compiler.getFs()->fileExists(packVfsPath);
    const bool diskBlobUpToDate = ShaderCompilerUtils::isCompiledShaderUpToDate(
        compiledFullPath,
        lastModifiedSourceCode);

    if (compiledBlobAvailable && (!compiler.canCompileShaders() || diskBlobUpToDate))
    {
        if (compiler.isVerbose())
        {
            caustica::info(
                "Using cached shader '%s' (%s)...",
                permutationName.c_str(),
                cacheKey.cacheFileNameNoExt().c_str());
        }
        return;
    }

    if (!compiler.canCompileShaders())
    {
        usCompileError = StringFormat(
            "Missing precompiled shader '%s' for permutation '%s' and runtime shader compilation is disabled.",
            compiledFullPath.c_str(),
            permutationName.c_str());
        std::string macroList;
        for (const auto& macro : combinedAndSpecializedMacros)
        {
            if (!macroList.empty())
                macroList += ", ";
            macroList += macro.name + "=" + macro.definition;
        }
        if (!macroList.empty())
            usCompileError += StringFormat(" Macros: [%s]", macroList.c_str());
        caustica::error("%s", usCompileError.c_str());
        return;
    }

    EnsureDirectoryExists(std::filesystem::path(compiledFullPath).parent_path());

    const std::filesystem::path pdbPath =
        compiler.getDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN || c_PTEmbedPdbs
        ? std::filesystem::path{}
        : std::filesystem::path(compiledFullPath).replace_extension(".pdb");

    usCompileCmdLine = makePathTracingShaderCompileCommand(
        compiler.getCompilerConfig(),
        buildResult,
        compiledFullPath,
        pdbPath);

    if (compiler.isVerbose())
        caustica::info("Enqueuing shader variant of '%s'...", srcFullPath.string().c_str());
}

PTPipelineVariant::PTPipelineVariant(const std::string & relativeSourcePath, const std::vector<caustica::ShaderMacro> & variantMacros, const std::shared_ptr<PathTracingShaderCompiler>& compiler, const std::string & shortUniqueDebugID, bool raygenOnly)
    : m_macros(variantMacros), m_compiler(compiler), m_rayGenOnly(raygenOnly)
{
    m_raygen.setPath(relativeSourcePath);

    // short unique ID distinguishes exports between pipeline variants and must be safe for HLSL entry point names
#if PIPELINE_BAKER_ENABLE_VERBOSE_FUNCTION_NAMING
    m_shortUniqueDebugID = StripNonAsciiAlnum(shortUniqueDebugID);
    assert( shortUniqueDebugID == m_shortUniqueDebugID ); // short unique debug ID must not contain any of the forbidden characters or bad things will happen, very bad
    if (m_shortUniqueDebugID.size()>6)
    {
        assert( false ); // too big
        m_shortUniqueDebugID = m_shortUniqueDebugID.substr(0, 6);
    }
    if (!compiler->registerShortUniqueDebugID(m_shortUniqueDebugID))
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
        baker->unregisterShortUniqueDebugID(m_shortUniqueDebugID);
}

void PTPipelineVariant::resetPipeline()
{
    m_shaderTable = nullptr;
    m_pipeline = nullptr;
    m_localVersion = -1;
}

void PTPipelineVariant::compileIfNeededEnqueue(std::filesystem::file_time_type lastModifiedSourceCode)
{
    std::shared_ptr<PathTracingShaderCompiler> & baker = m_lockedCompiler; assert( baker != nullptr );

    // no further specializations for these two
    m_raygen.combinedAndSpecializedMacros = m_combinedMacros;
    m_raygen.permutationName = m_shortUniqueDebugID + "_raygen";
    m_raygen.combinedAndSpecializedMacros.push_back( {"CAUSTICA_PIPELINE_PERMUTATION_NAME", m_shortUniqueDebugID } );  // used to rename ClosestHit (and, if any, AnyHit) to something readable in profilers and etc
    
    m_ubershaderMaterial.fromMaterialPermutation( m_shortUniqueDebugID, m_combinedMacros, *baker->getMaterialGpuCache()->getUbershader() );

    m_specializedPerMaterial.clear();
    const auto & sourceTable = baker->getMaterialGpuCache()->getShaderPermutationTable();
    m_specializedPerMaterial.resize(sourceTable.size());
    for( int i = 0; i < sourceTable.size(); i++ )
        m_specializedPerMaterial[i].fromMaterialPermutation( m_shortUniqueDebugID, m_combinedMacros, *(sourceTable[i]) );

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
        const std::string previousHashHex = permutation->cacheKey.cacheHashHex;
        permutation->resolveCacheIdentity(*baker, lastModifiedSourceCode);
        if (permutation->cacheKey.cacheHashHex != previousHashHex || !permutation->usCompileCmdLine.empty())
            resetPipelineNeeded = true;
        baker->enqueueShaderPermutation(permutation);
    }

    if (resetPipelineNeeded)
        resetPipeline();
}

//void PTPipelineVariant::CompileIfNeeded_Execute()
//{
//    std::shared_ptr<PathTracingShaderCompiler>& baker = m_lockedCompiler; assert(baker != nullptr);
//}

void PTPipelineVariant::updateStart(std::filesystem::file_time_type lastModifiedSourceCode)
{
    assert( m_lockedCompiler == nullptr );

    std::shared_ptr<PathTracingShaderCompiler> & compiler = m_lockedCompiler = m_compiler.lock();
    if (compiler == nullptr)
    {
        assert( false );
        return;
    }

    m_combinedMacros.clear(); m_combinedMacros.reserve( m_macros.size() + compiler->getMacros().size() );
    for (auto& macro : m_macros)
        m_combinedMacros.push_back(macro);
    for (auto& macro : compiler->getMacros())
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

    compileIfNeededEnqueue(lastModifiedSourceCode);
}

void PTPipelineVariant::updateFinalize()
{
    std::shared_ptr<PathTracingShaderCompiler> & baker = m_lockedCompiler; assert( baker != nullptr );

    // if no changes, no need to re-create pipeline
    if (m_pipeline == nullptr )
    {
        if (m_raygen.shaderLibrary == nullptr)
        {
            assert(false);
            return;
        }

        std::string rayGenName  = ("RayGen_"+m_shortUniqueDebugID);
        std::string missName    = ("Miss_"+m_shortUniqueDebugID);

        nvrhi::rt::PipelineDesc pipelineDesc;
        pipelineDesc.globalBindingLayouts = { baker->getBindingLayout(), baker->getBindlessLayout() };
        pipelineDesc.shaders.push_back({ "", m_raygen.shaderLibrary->getShader(rayGenName.c_str(), nvrhi::ShaderType::RayGeneration), nullptr });
        pipelineDesc.shaders.push_back({ "", m_raygen.shaderLibrary->getShader(missName.c_str(), nvrhi::ShaderType::Miss), nullptr });
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
        for (auto& [_, hitGroupInfo] : baker->getUniqueHitGroups())
        {
            std::string shaderPermutationName = hitGroupInfo.getShaderPermutationName();
            std::string closestHit = "ClosestHit_" + m_shortUniqueDebugID + "_" + shaderPermutationName;
            std::string anyHit = (m_exportAnyHit && hitGroupInfo.hasAnyHitShader)?("AnyHit_" + m_shortUniqueDebugID + "_" + shaderPermutationName):("");

#if PIPELINE_BAKER_REVERT_TO_UBERSHADER
            const ShaderPermutation & permutation = m_ubershaderMaterial;
#else
            const ShaderPermutation & permutation = m_specializedPerMaterial[hitGroupInfo.getShaderPermutationIndex()];
#endif
            if (permutation.shaderLibrary == nullptr)
            {
                assert(false);
                return;
            }
            pipelineDesc.hitGroups.push_back(
                {
                    .exportName = hitGroupInfo.getExportName(),
                    .closestHitShader = permutation.shaderLibrary->getShader(closestHit.c_str(), nvrhi::ShaderType::ClosestHit),
                    .anyHitShader = (anyHit != "") ? (permutation.shaderLibrary->getShader(anyHit.c_str(), nvrhi::ShaderType::AnyHit)) : (nullptr),
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
        if (usesNvapiHitObjectExtension && baker->isNvapiShaderExtensionEnabled())
            pipelineDesc.hlslExtensionsUAV = NV_SHADER_EXTN_SLOT_NUM;

        m_pipeline = baker->getDevice()->createRayTracingPipeline(pipelineDesc);

        if (!m_pipeline)
            { assert( false ); return; }

        m_shaderTable = m_pipeline->createShaderTable();

        if (!m_shaderTable)
            { assert( false ); return; }

        m_shaderTable->setRayGenerationShader(rayGenName.c_str());

        if (!m_rayGenOnly)
        {
            auto& perSubInstanceHitGroup = baker->getPerSubInstanceHitGroup();
            for (int i = 0; i < perSubInstanceHitGroup.size(); i++)
                m_shaderTable->addHitGroup(perSubInstanceHitGroup[i].getExportName().c_str());
        }

        if (m_exportMiss)
            m_shaderTable->addMissShader(missName.c_str());

        m_localVersion = baker->getVersion();

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
    if (!m_compilerConfig.initialize(device, c_PTShaderBinariesRoot))
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
        if (m_compilerConfig.canCompile())
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

std::string HitGroupInfo::getExportName() const { return "HitGroup_" + std::to_string(materialShaderPermutation->indexInTable); }


#if PIPELINE_BAKER_ENABLE_VERBOSE_FUNCTION_NAMING
std::string HitGroupInfo::getShaderPermutationName() const { return materialShaderPermutation->stableShaderName; }
#else
std::string HitGroupInfo::getShaderPermutationName() const { return materialShaderPermutation->stableShaderName; }
#endif

int HitGroupInfo::getShaderPermutationIndex() const { return materialShaderPermutation->indexInTable; }

HitGroupInfo ComputeSubInstanceHitGroupInfo(const MaterialGpuCache & baker, const PTMaterial& material)
{
    HitGroupInfo info;

#if PIPELINE_BAKER_REVERT_TO_UBERSHADER
    info.materialShaderPermutation = baker.getUbershader();
#else
    info.materialShaderPermutation = material.bakedShaderPermutation;
#endif

    info.hasAnyHitShader = material.hasAlphaTest();
    
    return info;
}

HitGroupInfo ComputeDefaultSubInstanceHitGroupInfo(const MaterialGpuCache& baker)
{
    HitGroupInfo info;
    info.materialShaderPermutation = baker.getUbershader();
    info.hasAnyHitShader = false;
    return info;
}

static bool macrosEqual(caustica::ShaderMacro& a, caustica::ShaderMacro& b)
{
    return a.name == b.name && a.definition == b.definition;
}

void PathTracingShaderCompiler::enqueueShaderPermutation(PTPipelineVariant::ShaderPermutation* perm)
{
    if (perm->usCompileCmdLine != "")
    {
        auto [it, inserted] = m_parallelCompileListUnique.try_emplace(perm->cacheKey.cacheFileNameNoExt(), perm);
        if (!inserted)
            perm->usMasterCopy = it->second;
    }
    m_parallelCompileListAll.push_back(perm);
}

void PathTracingShaderCompiler::update(const std::shared_ptr<caustica::Scene>& scene, unsigned int subInstanceCount, const std::function<void(std::vector<caustica::ShaderMacro>& macros)>& globalMacrosGetter, bool forceShaderReload)
{
    // Auto-reload: poll for source file changes
    if (m_compilerConfig.canCompile() && !forceShaderReload && !m_variants.empty())
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
        m_perSubInstanceHitGroup.assign(subInstanceCount, ComputeDefaultSubInstanceHitGroupInfo(*getMaterialGpuCache()));
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

                std::shared_ptr<PTMaterial> materialPT = PTMaterial::safeCast(proxy.mesh->geometries[gi]->material);
                if (!materialPT)
                    continue;

                m_perSubInstanceHitGroup[subInstanceIndex] = ComputeSubInstanceHitGroupInfo(*getMaterialGpuCache(), *materialPT);
            }
        }
        assert(m_perSubInstanceHitGroup.size() == subInstanceCount);

        // Prime the instances to make sure we only include the necessary CHS variants in the PSO. Many (sub)instances can map to same materials.
        m_uniqueHitGroups.clear();
        for (int i = 0; i < m_perSubInstanceHitGroup.size(); i++)
            m_uniqueHitGroups[m_perSubInstanceHitGroup[i].getShaderPermutationIndex()] = m_perSubInstanceHitGroup[i];
        needsUpdate = true;
    }

    if (needsUpdate)
    {
        m_version++;
    }

    if (needsUpdate)
    {
        if (m_compilerConfig.canCompile())
        {
            // we need the output folder
            EnsureDirectoryExists(m_compilerConfig.ShaderBinariesPath);
        }

        std::optional<std::filesystem::file_time_type> a = m_compilerConfig.canCompile()
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
                assert(false); // dangling Variant - forgotten a call to releaseVariant?
            if (variant->getVersion() != m_version)
            {
                updateQueue.push_back(variant);
                variant->resetPipeline();
                variant->updateStart(*m_lastUpdatedSourceTimestamp);
            }
        }

        std::atomic_int progressCounterCompleted;
        int progressTotal;

        ProgressBar progressCompilingShaders;
        progressCounterCompleted = 0;
        progressTotal = (int)m_parallelCompileListUnique.size();
        if (m_parallelCompileListUnique.size() > 0)
            progressCompilingShaders.Start(StringFormat("Compiling shaders (%d)...", progressTotal).c_str());
        else if (isLoadOnlyMode() && !updateQueue.empty())
            caustica::info("PathTracingShaderCompiler: loading precompiled RT shader libraries...");

        for (auto it : m_parallelCompileListUnique)
        {
            PTPipelineVariant::ShaderPermutation* permutation = it.second;
            if (permutation->usCompileCmdLine == "")
            {
                assert(false); continue;
            } // not sure why this would happen

#if BAKER_ENABLE_MULTITHREADED_COMPILE_SHADER
            m_threadPool.AddTask([ this, permutation, &progressCompilingShaders, &progressCounterCompleted, progressTotal ]() {
#endif
                permutation->compileIfNeeded();
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
            if (permutation->usCompileError != "")
            {
                firstError = permutation->usCompileError;
                break;
            }

            permutation->loadShaderLibraryIfNeeded(*this);
            // if error, break & skip
            if (permutation->shaderLibrary == nullptr)
            {
                firstError = permutation->usCompileError;
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
                variant->updateFinalize();
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

void PathTracingShaderCompiler::releaseVariant(std::shared_ptr<PTPipelineVariant>& variant)
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

std::shared_ptr<PTPipelineVariant> PathTracingShaderCompiler::createVariant(const std::string & relativeSourcePath, std::vector<caustica::ShaderMacro> variantMacros, const std::string & shortUniqueDebugID, bool rayGenOnly)
{
    std::shared_ptr<PTPipelineVariant> variant = std::shared_ptr<PTPipelineVariant>(new PTPipelineVariant(relativeSourcePath, variantMacros, this->shared_from_this(), shortUniqueDebugID, rayGenOnly));
    m_variants.push_back(variant);
    return variant;
}

bool PathTracingShaderCompiler::registerShortUniqueDebugID(const std::string& id)
{
    auto [it, inserted] = m_shortUniqueDebugIDs.insert(id);

    return inserted;
}

void PathTracingShaderCompiler::unregisterShortUniqueDebugID(const std::string& id)
{
    int count = m_shortUniqueDebugIDs.erase(id);
    assert( count > 0 );
}
