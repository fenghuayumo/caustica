#pragma once

#include <mutex>
#include <memory>
#include <rhi/rhi.h>
#include <assets/loader/ShaderFactory.h>
#include <core/ThreadPool.h>
#include <unordered_map>

#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>

namespace caustica
{
    class RootFileSystem;
}

class ComputeShaderVariant;

class ComputePipelineRegistry : public std::enable_shared_from_this<ComputePipelineRegistry>
{
public:
    // additionalMonitorPaths: extra directories to monitor for hot reload (in addition to the default ShadersPath)
    ComputePipelineRegistry(caustica::rhi::IDevice* device, const std::vector<std::filesystem::path>& additionalMonitorPaths = {});
    ~ComputePipelineRegistry();

    // Register a compute shader for hot reload management
    // Returns a variant handle that can be used to get the pipeline
    std::shared_ptr<ComputeShaderVariant> createVariant(
        const std::string& shaderSourcePath,
        const std::string& entryPoint,
        const std::vector<caustica::ShaderMacro>& macros,
        const caustica::rhi::BindingLayoutVector& bindingLayouts,
        const std::string& debugName);

    // Release a previously created variant
    void releaseVariant(std::shared_ptr<ComputeShaderVariant>& variant);

    // Called each frame - checks for changes and recompiles if needed
    // Set forceReload=true to force recompilation of all shaders
    void update(bool forceReload = false);

    // Access to verbose mode for debugging
    bool isVerbose() const { return m_verbose; }
    void setVerbose(bool verbose) { m_verbose = verbose; }
    bool canCompileShaders() const { return m_compilerConfig.canCompile(); }
    bool isLoadOnlyMode() const { return !m_compilerConfig.canCompile(); }

private:
    friend class ComputeShaderVariant;

    caustica::rhi::DeviceHandle getDevice() const { return m_device; }
    
    const ShaderCompilerUtils::ShaderCompilerConfig& getCompilerConfig() const { return m_compilerConfig; }
    
    const std::filesystem::path& getShaderBinariesPath() const { return m_compilerConfig.ShaderBinariesPath; }
    const std::filesystem::path& getShaderCompilerPath() const { return m_compilerConfig.ShaderCompilerPath; }
    const std::filesystem::path& getShadersPath() const { return m_compilerConfig.ShadersPath; }
    const std::filesystem::path& getShadersPathExternalIncludes1() const { return m_compilerConfig.ShadersPathExternalIncludes1; }
    const std::filesystem::path& getShadersPathExternalIncludes2() const { return m_compilerConfig.ShadersPathExternalIncludes2; }
    
    std::shared_ptr<caustica::RootFileSystem> getFS() { return m_shadersFS; }
    std::mutex& getMutex() { return m_mutex; }

    int64_t getVersion() const { return m_version; }

    // Enqueue a shader for compilation (thread-safe)
    void enqueueShaderForCompilation(ComputeShaderVariant* variant);

private:
    caustica::rhi::DeviceHandle m_device;
    std::shared_ptr<caustica::RootFileSystem> m_shadersFS;
    
    ShaderCompilerUtils::ShaderCompilerConfig m_compilerConfig;
    
    std::vector<std::shared_ptr<ComputeShaderVariant>> m_variants;
    
    // For parallel compilation
    std::vector<ComputeShaderVariant*> m_parallelCompileListAll;
    std::unordered_map<std::string, ComputeShaderVariant*> m_parallelCompileListUnique;
    
    caustica::ThreadPool m_threadPool;
    
    std::optional<std::filesystem::file_time_type> m_lastUpdatedSourceTimestamp;
    
    bool m_verbose = false;
    
    int64_t m_version = 0;
    
    std::mutex m_mutex;

    // Auto-reload via polling
    float m_autoReloadPollIntervalSeconds = 0.5f;
    std::optional<std::filesystem::file_time_type> m_cachedSourceTimestamp;
    
    // Additional directories to monitor for hot reload (besides ShadersPath)
    std::vector<std::filesystem::path> m_additionalMonitorPaths;
};

class ComputeShaderVariant
{
public:
    ~ComputeShaderVariant();

    // Get the compiled compute pipeline
    // Returns nullptr if pipeline is not yet compiled or compilation failed
    caustica::rhi::ComputePipelineHandle getPipeline() const { return m_pipeline; }

    // Check if the variant needs to be updated (recompiled)
    bool needsUpdate() const;

    // Get the last compilation error (empty if no error)
    const std::string& getCompileError() const { return m_compileError; }

    // Get debug name
    const std::string& getDebugName() const { return m_debugName; }

private:
    friend class ComputePipelineRegistry;

    ComputeShaderVariant(
        const std::string& shaderSourcePath,
        const std::string& entryPoint,
        const std::vector<caustica::ShaderMacro>& macros,
        const caustica::rhi::BindingLayoutVector& bindingLayouts,
        const std::string& debugName,
        const std::shared_ptr<ComputePipelineRegistry>& registry);

    // Prepare compilation command (does not execute)
    void prepareCompilation(std::filesystem::file_time_type lastModifiedSourceCode);

    // execute the actual compilation (called from thread pool)
    void compileIfNeeded();

    // load shader blob and create pipeline
    void loadShaderAndCreatePipeline();

    // reset pipeline (forces recompile on next update)
    void resetPipeline();

private:
    std::weak_ptr<ComputePipelineRegistry> m_registry;
    std::shared_ptr<ComputePipelineRegistry> m_lockedRegistry; // temporary lock during update

    // Shader source info
    std::filesystem::path m_shaderSrcFileName;
    std::string m_entryPoint;
    std::vector<caustica::ShaderMacro> m_macros;
    caustica::rhi::BindingLayoutVector m_bindingLayouts;
    std::string m_debugName;

    // Compiled shader info
    caustica::ShaderKey m_cacheKey;
    std::string m_compiledFullPath;
    std::string m_packVfsPath;
    std::string m_compileCmdLine;
    std::string m_compileError;

    // Pipeline
    caustica::rhi::ShaderHandle m_shader;
    caustica::rhi::ComputePipelineHandle m_pipeline;

    // Version tracking
    int64_t m_localVersion = -1;
};
