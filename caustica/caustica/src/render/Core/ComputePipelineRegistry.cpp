#include <render/Core/ComputePipelineRegistry.h>
#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>
#include <assets/loader/ShaderPackFileSystem.h>

#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/log.h>
#include <core/vfs/VFS.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/system_utils.h>

using namespace caustica;

#define COMPUTE_REGISTRY_ENABLE_MULTITHREADED_COMPILE 1
#define COMPUTE_REGISTRY_EMBED_PDBS 0
#define COMPUTE_REGISTRY_USE_OPTIMIZATIONS 1

using namespace caustica;

static const std::string c_ComputeShaderBinariesRoot = "ShaderDynamic/Bin";
static const std::string c_ComputeShaderPackMount = "/" + c_ComputeShaderBinariesRoot;

//////////////////////////////////////////////////////////////////////////
// ComputePipelineRegistry implementation
//////////////////////////////////////////////////////////////////////////

ComputePipelineRegistry::ComputePipelineRegistry(nvrhi::IDevice* device, const std::vector<std::filesystem::path>& additionalMonitorPaths)
    : m_device(device)
{
    if (!m_compilerConfig.Initialize(device, c_ComputeShaderBinariesRoot))
        caustica::fatal("Failed to initialize compute shader compiler configuration");

    m_shadersFS = std::make_shared<caustica::RootFileSystem>();
    const char* shaderTypeName = caustica::GetShaderTypeName(device->getGraphicsAPI());
    const std::filesystem::path shaderPackPath = GetRuntimeDirectory() / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, c_ComputeShaderBinariesRoot);
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
        m_shadersFS->mount("/" + c_ComputeShaderBinariesRoot, shaderPackFS);
        m_compilerConfig.RuntimeCompilationAvailable = false;
        caustica::info(
            "ComputePipelineRegistry: load-only mode using shader pack '%s'.",
            shaderPackPath.string().c_str());
    }
    else
    {
        m_shadersFS->mount("/" + c_ComputeShaderBinariesRoot, m_compilerConfig.ShaderBinariesPath);
        if (m_compilerConfig.CanCompile())
            caustica::info("ComputePipelineRegistry: dev mode — runtime DXC compilation enabled.");
        else
            caustica::info("ComputePipelineRegistry: load-only mode using '%s'.", m_compilerConfig.ShaderBinariesPath.string().c_str());
    }
    
    // Store additional paths to monitor (converted to absolute)
    for (const auto& path : additionalMonitorPaths)
    {
        m_additionalMonitorPaths.push_back(std::filesystem::absolute(path));
    }
    
    caustica::info("ComputePipelineRegistry initialized (monitoring %d additional paths)", (int)m_additionalMonitorPaths.size());
}

ComputePipelineRegistry::~ComputePipelineRegistry()
{
}

std::shared_ptr<ComputeShaderVariant> ComputePipelineRegistry::CreateVariant(
    const std::string& shaderSourcePath,
    const std::string& entryPoint,
    const std::vector<ShaderMacro>& macros,
    const nvrhi::BindingLayoutVector& bindingLayouts,
    const std::string& debugName)
{
    std::shared_ptr<ComputeShaderVariant> variant = std::shared_ptr<ComputeShaderVariant>(
        new ComputeShaderVariant(shaderSourcePath, entryPoint, macros, bindingLayouts, debugName, shared_from_this()));
    m_variants.push_back(variant);
    return variant;
}

void ComputePipelineRegistry::ReleaseVariant(std::shared_ptr<ComputeShaderVariant>& variant)
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
    assert(false); // Variant not found
}

void ComputePipelineRegistry::EnqueueShaderForCompilation(ComputeShaderVariant* variant)
{
    if (variant->m_compileCmdLine != "")
    {
        auto [it, inserted] = m_parallelCompileListUnique.try_emplace(variant->m_cacheKey.cacheFileNameNoExt(), variant);
        // If not inserted, it means another variant with the same output already exists
        // (unlikely for compute shaders with unique debug names, but handled for safety)
    }
    m_parallelCompileListAll.push_back(variant);
}

void ComputePipelineRegistry::Update(bool forceReload)
{
    // Auto-reload: poll for source file changes
    if (m_compilerConfig.CanCompile() && !forceReload && !m_variants.empty())
    {
        static auto lastPollTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastPollTime).count();
        
        if (elapsed >= m_autoReloadPollIntervalSeconds)
        {
            lastPollTime = now;
            
            // Check the main shaders path
            auto currentTimestamp = GetLatestModifiedTimeDirectoryRecursive(m_compilerConfig.ShadersPath);
            
            // Also check additional monitored paths
            for (const auto& path : m_additionalMonitorPaths)
            {
                if (std::filesystem::exists(path))
                {
                    auto pathTimestamp = GetLatestModifiedTimeDirectoryRecursive(path);
                    if (pathTimestamp.has_value())
                    {
                        if (!currentTimestamp.has_value() || *pathTimestamp > *currentTimestamp)
                            currentTimestamp = pathTimestamp;
                    }
                }
            }
            
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
                    forceReload = true;
                    caustica::info("Compute shader source changes detected - triggering hot reload...");
                }
            }
        }
    }

    // Check if any variants need updating
    bool needsUpdate = forceReload;
    
    for (const auto& variant : m_variants)
    {
        if (variant.use_count() == 1)
        {
            assert(false); // Dangling variant - forgotten a call to ReleaseVariant?
        }
        if (variant->NeedsUpdate())
        {
            needsUpdate = true;
            break;
        }
    }

    if (!needsUpdate && !forceReload)
        return;

    // Get latest source modification time (from all monitored paths)
    if (m_compilerConfig.CanCompile())
        EnsureDirectoryExists(m_compilerConfig.ShaderBinariesPath);
    
    m_lastUpdatedSourceTimestamp = m_compilerConfig.CanCompile()
        ? GetLatestModifiedTimeDirectoryRecursive(m_compilerConfig.ShadersPath)
        : std::optional<std::filesystem::file_time_type>(std::filesystem::file_time_type::min());
    
    // Also include additional monitored paths
    if (m_compilerConfig.CanCompile())
    {
        for (const auto& path : m_additionalMonitorPaths)
        {
            if (std::filesystem::exists(path))
            {
                auto pathTimestamp = GetLatestModifiedTimeDirectoryRecursive(path);
                if (pathTimestamp.has_value())
                {
                    if (!m_lastUpdatedSourceTimestamp.has_value() || *pathTimestamp > *m_lastUpdatedSourceTimestamp)
                        m_lastUpdatedSourceTimestamp = pathTimestamp;
                }
            }
        }
    }
    
    if (!m_lastUpdatedSourceTimestamp.has_value())
    {
        caustica::error("Unable to get source timestamp - cannot load or dynamically compile compute shaders");
        return;
    }

    m_version++;

    // Compilation loop with retry on error
    do
    {
        assert(m_parallelCompileListAll.empty() && m_parallelCompileListUnique.empty());

        std::vector<std::shared_ptr<ComputeShaderVariant>> updateQueue;
        
        for (auto& variant : m_variants)
        {
            if (variant->m_localVersion != m_version || forceReload)
            {
                updateQueue.push_back(variant);
                variant->ResetPipeline();
                variant->m_lockedRegistry = shared_from_this();
                variant->PrepareCompilation(*m_lastUpdatedSourceTimestamp);
                EnqueueShaderForCompilation(variant.get());
            }
        }

        if (updateQueue.empty())
            break;

        // Progress tracking
        std::atomic_int progressCounterCompleted;
        int progressTotal;

        ProgressBar progressCompilingShaders;
        progressCounterCompleted = 0;
        progressTotal = (int)m_parallelCompileListUnique.size();
        
        if (progressTotal > 0)
            progressCompilingShaders.Start(StringFormat("Compiling compute shaders (%d)...", progressTotal).c_str());

        // Compile shaders (potentially in parallel)
        for (auto& [name, variant] : m_parallelCompileListUnique)
        {
            if (variant->m_compileCmdLine == "")
            {
                assert(false);
                continue;
            }

#if COMPUTE_REGISTRY_ENABLE_MULTITHREADED_COMPILE
            m_threadPool.AddTask([this, variant, &progressCompilingShaders, &progressCounterCompleted, progressTotal]() {
#endif
                variant->CompileIfNeeded();
                int completed = progressCounterCompleted.fetch_add(1) + 1;
                progressCompilingShaders.Set(100 * completed / progressTotal);
#if COMPUTE_REGISTRY_ENABLE_MULTITHREADED_COMPILE
            });
#endif
        }

#if COMPUTE_REGISTRY_ENABLE_MULTITHREADED_COMPILE
        m_threadPool.WaitForTasks();
#endif

        // Check for errors and load shaders
        std::string firstError = "";

        for (ComputeShaderVariant* variant : m_parallelCompileListAll)
        {
            if (variant->m_compileError != "")
            {
                firstError = variant->m_compileError;
                break;
            }

            variant->LoadShaderAndCreatePipeline();
            
            if (variant->m_compileError != "")
            {
                firstError = variant->m_compileError;
                break;
            }
        }

        progressCompilingShaders.Set(100);
        m_parallelCompileListAll.clear();
        m_parallelCompileListUnique.clear();

        // Mark variants as updated
        for (auto& variant : updateQueue)
        {
            if (firstError.empty())
                variant->m_localVersion = m_version;
            variant->m_lockedRegistry = nullptr;
        }

        if (!firstError.empty())
        {
            caustica::error("%s", firstError.c_str());
            bool retry = false;
#if _WIN32
            if (!HelpersIsNonInteractive())
            {
                int result = MessageBoxA((HWND)HelpersGetActiveWindow(), firstError.c_str(),
                    "Compute Shader Compile Error", MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TASKMODAL);
                retry = result != IDCANCEL;
            }
#endif
            if (!retry)
                break;
        }
        else
        {
            break;
        }

    } while (true);
}

//////////////////////////////////////////////////////////////////////////
// ComputeShaderVariant implementation
//////////////////////////////////////////////////////////////////////////

ComputeShaderVariant::ComputeShaderVariant(
    const std::string& shaderSourcePath,
    const std::string& entryPoint,
    const std::vector<ShaderMacro>& macros,
    const nvrhi::BindingLayoutVector& bindingLayouts,
    const std::string& debugName,
    const std::shared_ptr<ComputePipelineRegistry>& registry)
    : m_registry(registry)
    , m_shaderSrcFileName(shaderSourcePath)
    , m_entryPoint(entryPoint)
    , m_macros(macros)
    , m_bindingLayouts(bindingLayouts)
    , m_debugName(debugName)
{
}

ComputeShaderVariant::~ComputeShaderVariant()
{
}

bool ComputeShaderVariant::NeedsUpdate() const
{
    auto registry = m_registry.lock();
    if (!registry)
        return false;
    return m_localVersion != registry->GetVersion() || m_pipeline == nullptr;
}

void ComputeShaderVariant::ResetPipeline()
{
    m_shader = nullptr;
    m_pipeline = nullptr;
    m_localVersion = -1;
}

void ComputeShaderVariant::PrepareCompilation(std::filesystem::file_time_type lastModifiedSourceCode)
{
    auto registry = m_lockedRegistry;
    assert(registry != nullptr);

    // Clear any previous error
    m_compileError = "";
    m_compileCmdLine = "";

    // Build full source path
    auto srcFullPath = std::filesystem::absolute(registry->GetShadersPath() / m_shaderSrcFileName);

    // Build DXC command using shared utilities
    ShaderCompilerUtils::DxcCommandOptions options;
    options.SourceFilePath = srcFullPath;
    options.LogicalSourceFileName = m_shaderSrcFileName;
#if CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619
    options.Profile = ShaderCompilerUtils::ShaderProfile::Compute_6_9;
#else
    options.Profile = ShaderCompilerUtils::ShaderProfile::Compute_6_6;
#endif
    options.EntryPoint = m_entryPoint;
    options.EnableDebugInfo = true;
    options.EmbedPdb = COMPUTE_REGISTRY_EMBED_PDBS != 0;
    options.UseOptimizations = COMPUTE_REGISTRY_USE_OPTIMIZATIONS != 0;
    options.Enable16BitTypes = true;
    options.WarningsAsErrors = true;
    options.AllResourcesBound = true;
    options.EnableDebugPrint = true;
    options.Macros = m_macros;

    auto cmdResult = ShaderCompilerUtils::BuildDxcCommand(registry->GetCompilerConfig(), options);
    caustica::ShaderKey cacheKey = ShaderCompilerUtils::MakeShaderKey(registry->GetCompilerConfig(), options);
    cacheKey.cacheHashHex = cmdResult.HashHex;

    const std::string previousHashHex = m_cacheKey.cacheHashHex;
    m_cacheKey = std::move(cacheKey);
    if (m_cacheKey.cacheHashHex != previousHashHex)
        ResetPipeline();

    m_compiledFullPath = m_cacheKey.cacheFilePath(registry->GetShaderBinariesPath()).string();
    m_packVfsPath = m_cacheKey.packVfsPath(c_ComputeShaderPackMount);

    const bool compiledBlobAvailable = registry->GetFS()->fileExists(m_packVfsPath);
    const bool diskBlobUpToDate = ShaderCompilerUtils::IsCompiledShaderUpToDate(m_compiledFullPath, lastModifiedSourceCode);
    if (compiledBlobAvailable && (!registry->CanCompileShaders() || diskBlobUpToDate))
    {
        if (registry->IsVerbose())
            caustica::info("Using cached compute shader '%s' (%s)", m_debugName.c_str(), m_cacheKey.cacheFileNameNoExt().c_str());
        m_compileCmdLine = "";
    }
    else if (registry->CanCompileShaders())
    {
        EnsureDirectoryExists(std::filesystem::path(m_compiledFullPath).parent_path());
        std::string command = registry->GetCompilerConfig().GetCompilerPathQuoted();
        command += cmdResult.CommandBase;

#if !COMPUTE_REGISTRY_EMBED_PDBS
        if (registry->GetCompilerConfig().GraphicsAPI != nvrhi::GraphicsAPI::VULKAN)
        {
            std::filesystem::path pdbPath = m_compiledFullPath;
            pdbPath.replace_extension(".pdb");
            command += " /Fd \"" + pdbPath.string() + "\"";
        }
#endif
        command += " -Fo \"" + m_compiledFullPath + "\"";

        if (registry->IsVerbose())
            caustica::info("Enqueuing compute shader '%s' for compilation...", m_debugName.c_str());

        m_compileCmdLine = command;
        ResetPipeline();
    }
    else
    {
        m_compileError = StringFormat(
            "Missing precompiled compute shader '%s' and runtime shader compilation is disabled.",
            m_compiledFullPath.c_str());
        caustica::error("%s", m_compileError.c_str());
    }
}

void ComputeShaderVariant::CompileIfNeeded()
{
    if (m_compileCmdLine.empty())
        return;

    caustica::info("Compiling compute shader '%s'...", m_debugName.c_str());

    auto [resNum, resString, resErrorString] = SystemShell(m_compileCmdLine, false);

    m_compileError = "";

    if (!resErrorString.empty())
    {
        m_compileError = StringFormat("ERROR compiling compute shader '%s':\nCommand: %s\nError: %s",
            m_debugName.c_str(), m_compileCmdLine.c_str(), resErrorString.c_str());
    }

    if (!m_compileError.empty())
        caustica::warning("%s", m_compileError.c_str());
}

void ComputeShaderVariant::LoadShaderAndCreatePipeline()
{
    if (!m_compileError.empty() || m_pipeline != nullptr)
        return;

    auto registry = m_lockedRegistry;
    assert(registry != nullptr);

    std::shared_ptr<caustica::IBlob> data = registry->GetFS()->readFile(m_packVfsPath.c_str());

    if (!data)
    {
        m_compileError = StringFormat(
            "Failed to load compiled compute shader '%s' (pack path '%s').",
            m_debugName.c_str(),
            m_packVfsPath.c_str());
        return;
    }

    // Create shader
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.debugName = m_debugName;
    shaderDesc.entryName = m_entryPoint.c_str();

    m_shader = registry->GetDevice()->createShader(shaderDesc, data->data(), data->size());

    if (!m_shader)
    {
        m_compileError = StringFormat("ERROR creating compute shader handle for '%s'", m_debugName.c_str());
        return;
    }

    // Create pipeline
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_shader;
    pipelineDesc.bindingLayouts = m_bindingLayouts;

    m_pipeline = registry->GetDevice()->createComputePipeline(pipelineDesc);

    if (!m_pipeline)
    {
        m_compileError = StringFormat("ERROR creating compute pipeline for '%s'", m_debugName.c_str());
        m_shader = nullptr;
        return;
    }

    caustica::info("Successfully created compute pipeline for '%s'", m_debugName.c_str());
}
