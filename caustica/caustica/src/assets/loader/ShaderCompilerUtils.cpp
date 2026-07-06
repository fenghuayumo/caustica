#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderBackend.h>
#include <assets/loader/ShaderKey.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/picosha2.h>

#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/log.h>

using namespace caustica;

namespace ShaderCompilerUtils
{
    //////////////////////////////////////////////////////////////////////////
    // SHA256 hash computation
    //////////////////////////////////////////////////////////////////////////
    
    static_assert(picosha2::k_digest_size == k_Sha256DigestSize, "SHA256 digest size mismatch");
    
    std::string computeSha256Hex(const std::string& input)
    {
        std::vector<unsigned char> hash(picosha2::k_digest_size);
        picosha2::hash256(input.begin(), input.end(), hash.begin(), hash.end());
        return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
    }
    
    //////////////////////////////////////////////////////////////////////////
    // Shader compiler configuration
    //////////////////////////////////////////////////////////////////////////
    
    bool ShaderCompilerConfig::initialize(nvrhi::IDevice* device, const std::string& binarySubfolder)
    {
        std::string graphicsAPIName;
        if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
            graphicsAPIName = "d3d12";
        else if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            graphicsAPIName = "vk";
        else
        {
            caustica::error("Unsupported graphics API for shader compilation");
            return false;
        }
        
        GraphicsAPI = device->getGraphicsAPI();
        
        std::string platformName = "x64"; // add "arm64" for ARM
        const std::filesystem::path runtimeDirectory = GetRuntimeDirectory();
        const std::filesystem::path sourceRootDirectory = runtimeDirectory.parent_path();
        
    #if defined(_WIN32)
        const char* dxcExecutableName = "dxc.exe";
    #else
        const char* dxcExecutableName = "dxc";
    #endif

        std::filesystem::path shaderSourcePathDevelopment =
            sourceRootDirectory / "caustica/caustica/shaders";
        std::filesystem::path shaderSourcePathRuntime =
            runtimeDirectory / "ShaderDynamic/Source/caustica/caustica/shaders";

        ShaderBinariesPath = runtimeDirectory / binarySubfolder / 
            caustica::GetShaderTypeName(device->getGraphicsAPI());

        ShaderCompilerPath = std::filesystem::absolute(
            runtimeDirectory / "ShaderDynamic/Tools" / graphicsAPIName / platformName / dxcExecutableName);
        
        if (!std::filesystem::exists(shaderSourcePathDevelopment))
        {
            caustica::info("Shaders development folder '%s' not found, trying local '%s'...", 
                shaderSourcePathDevelopment.string().c_str(), 
                shaderSourcePathRuntime.string().c_str());
                
            if (!std::filesystem::exists(shaderSourcePathRuntime))
            {
                caustica::info("Shader source folder '%s' not found; runtime shader compilation is disabled.",
                    shaderSourcePathRuntime.string().c_str());
                ShadersPath = shaderSourcePathRuntime;
                ShadersPathExternalIncludes1 = runtimeDirectory / "ShaderDynamic/Source/caustica/caustica";
                ShadersPathExternalIncludes2 = runtimeDirectory / "ShaderDynamic/Source/External";
            }
            else
            {
                ShadersPath = shaderSourcePathRuntime;
                ShadersPathExternalIncludes1 = runtimeDirectory / "ShaderDynamic/Source/caustica/caustica";
                ShadersPathExternalIncludes2 = runtimeDirectory / "ShaderDynamic/Source/External";
            }
        }
        else
        {
            ShadersPath = shaderSourcePathDevelopment;
            ShadersPathExternalIncludes1 =
                sourceRootDirectory / "caustica/caustica";
            ShadersPathExternalIncludes2 =
                sourceRootDirectory / "External";
        }

        // Convert all paths to absolute
        ShaderCompilerPath = std::filesystem::absolute(ShaderCompilerPath);
        ShadersPath = std::filesystem::absolute(ShadersPath);
        ShadersPathExternalIncludes1 = std::filesystem::absolute(ShadersPathExternalIncludes1);
        if (!ShadersPathExternalIncludes2.empty())
            ShadersPathExternalIncludes2 = std::filesystem::absolute(ShadersPathExternalIncludes2);
        ShaderBinariesPath = std::filesystem::absolute(ShaderBinariesPath);

#if CAUSTICA_WITH_RUNTIME_SHADER_COMPILATION
        RuntimeCompilationAvailable =
            std::filesystem::exists(ShaderCompilerPath) &&
            std::filesystem::exists(ShadersPath);

        if (!RuntimeCompilationAvailable)
        {
            if (!std::filesystem::exists(ShaderCompilerPath))
            {
                caustica::info("Shader compiler '%s' not found; loading precompiled shader binaries only.",
                    ShaderCompilerPath.string().c_str());
            }
        }
        
        if (RuntimeCompilationAvailable)
        {
            caustica::info("Shader compiler: '%s'", ShaderCompilerPath.string().c_str());
            caustica::info("Shader sources: '%s' (includes: '%s', '%s')",
                ShadersPath.string().c_str(),
                ShadersPathExternalIncludes1.string().c_str(),
                ShadersPathExternalIncludes2.string().c_str());
        }
#else
        RuntimeCompilationAvailable = false;
        caustica::info("Runtime shader compilation disabled; loading precompiled shader binaries from '%s'.",
            ShaderBinariesPath.string().c_str());
#endif
        caustica::info("Shader binaries output: '%s'", ShaderBinariesPath.string().c_str());
        
        return true;
    }
    
    std::string ShaderCompilerConfig::getCompilerPathQuoted() const
    {
        return "\"" + ShaderCompilerPath.string() + "\"";
    }
    
    //////////////////////////////////////////////////////////////////////////
    // DXC command-line builder
    //////////////////////////////////////////////////////////////////////////
    
    static std::string GetProfileString(ShaderProfile profile)
    {
        switch (profile)
        {
        case ShaderProfile::Library_6_6:    return "lib_6_6";
        case ShaderProfile::Library_6_9:    return "lib_6_9";
        case ShaderProfile::Compute_6_6:    return "cs_6_6";
        case ShaderProfile::Compute_6_9:    return "cs_6_9";
        default:                            return "cs_6_6";
        }
    }
    
    static bool IsLibraryProfile(ShaderProfile profile)
    {
        return profile == ShaderProfile::Library_6_6 || profile == ShaderProfile::Library_6_9;
    }
    
    DxcCommandResult buildDxcCommand(
        const ShaderCompilerConfig& config,
        const DxcCommandOptions& options)
    {
        DxcCommandResult result;
        
        auto srcFullPath = std::filesystem::absolute(options.SourceFilePath);
        std::filesystem::path logicalSource = options.LogicalSourceFileName.empty()
            ? options.SourceFilePath.filename()
            : options.LogicalSourceFileName;
        
        // See https://simoncoenen.com/blog/programming/graphics/DxcCompiling for switch reference
        std::string command;
        std::string hashCommand;
        
        // Source file
        command += " \"" + srcFullPath.string() + "\"";
        hashCommand += " \"" + logicalSource.generic_string() + "\"";
        
        // Debug info
        if (options.EnableDebugInfo)
        {
            command += " -Zi";              // Enable debug information
            hashCommand += " -Zi";
            if (options.EmbedPdb)
            {
                command += " -Qembed_debug"; // Embed PDB in shader container
                hashCommand += " -Qembed_debug";
            }
        }
        
        // Hash based on binary output only
        command += " -Zsb";
        hashCommand += " -Zsb";
        
        // Optimization level
        if (options.UseOptimizations)
        {
            command += " -O3";
            hashCommand += " -O3";
        }
        else
        {
            command += " -Od";
            hashCommand += " -Od";
        }
        
        // 16-bit types
        if (options.Enable16BitTypes)
        {
            command += " -enable-16bit-types";
            hashCommand += " -enable-16bit-types";
        }
        
        // Warnings as errors
        if (options.WarningsAsErrors)
        {
            command += " -WX";
            hashCommand += " -WX";
        }
        
        // All resources bound
        if (options.AllResourcesBound)
        {
            command += " -all_resources_bound";
            hashCommand += " -all_resources_bound";
        }
        
        // Shader profile/target
        command += " -T " + GetProfileString(options.Profile);
        hashCommand += " -T " + GetProfileString(options.Profile);
        
        // Entry point (only for non-library targets)
        if (!IsLibraryProfile(options.Profile) && !options.EntryPoint.empty())
        {
            command += " -E " + options.EntryPoint;
            hashCommand += " -E " + options.EntryPoint;
        }
        
        // Payload qualifiers for older library profiles
        if (options.Profile == ShaderProfile::Library_6_6)
        {
            command += " -enable-payload-qualifiers";
            hashCommand += " -enable-payload-qualifiers";
        }
        
        // Debug print macro
        if (options.EnableDebugPrint)
        {
            command += " -D ENABLE_DEBUG_PRINT";
            hashCommand += " -D ENABLE_DEBUG_PRINT";
        }
        
        // User-defined macros
        for (const auto& macro : options.Macros)
        {
            command += " -D " + macro.name + "=" + macro.definition;
            hashCommand += " -D " + macro.name + "=" + macro.definition;
        }
        
        // Include paths - config's external includes
        command += " -I \"" + config.ShadersPathExternalIncludes1.string() + "\"";
        hashCommand += " -I <external1>";
        if (!config.ShadersPathExternalIncludes2.empty())
            command += " -I \"" + config.ShadersPathExternalIncludes2.string() + "\"";
        hashCommand += " -I <external2>";
        
        // Additional include paths
        for (const auto& includePath : options.AdditionalIncludes)
        {
            command += " -I \"" + includePath.string() + "\"";
            hashCommand += " -I <additional:" + includePath.filename().generic_string() + ">";
        }
        
        // Target API macro
        std::string targetMacro = " -D ";
        if (config.GraphicsAPI == nvrhi::GraphicsAPI::D3D12)
        {
            targetMacro += "TARGET_D3D12";
        }
        else if (config.GraphicsAPI == nvrhi::GraphicsAPI::VULKAN)
        {
            targetMacro += "TARGET_VULKAN";
        }
        command += targetMacro;
        hashCommand += targetMacro;
        
        // Vulkan-specific options
        if (config.GraphicsAPI == nvrhi::GraphicsAPI::VULKAN)
        {
            command += " -D SPIRV";
            command += " -spirv";
            command += " -fspv-target-env=vulkan1.2";
            command += " -fspv-extension=SPV_EXT_descriptor_indexing";
            command += " -fspv-extension=KHR";
            hashCommand += " -D SPIRV";
            hashCommand += " -spirv";
            hashCommand += " -fspv-target-env=vulkan1.2";
            hashCommand += " -fspv-extension=SPV_EXT_descriptor_indexing";
            hashCommand += " -fspv-extension=KHR";
            
            nvrhi::VulkanBindingOffsets cBindingOffsets;
            for (int i = 0; i < 7; i++)
            {
                // TODO: test with 'all' instead of the second %d - should work as well
                command += StringFormat(" -fvk-s-shift %d %d", cBindingOffsets.sampler, i);
                command += StringFormat(" -fvk-t-shift %d %d", cBindingOffsets.shaderResource, i);
                command += StringFormat(" -fvk-b-shift %d %d", cBindingOffsets.constantBuffer, i);
                command += StringFormat(" -fvk-u-shift %d %d", cBindingOffsets.unorderedAccess, i);
                hashCommand += StringFormat(" -fvk-s-shift %d %d", cBindingOffsets.sampler, i);
                hashCommand += StringFormat(" -fvk-t-shift %d %d", cBindingOffsets.shaderResource, i);
                hashCommand += StringFormat(" -fvk-b-shift %d %d", cBindingOffsets.constantBuffer, i);
                hashCommand += StringFormat(" -fvk-u-shift %d %d", cBindingOffsets.unorderedAccess, i);
            }
        }
        
        result.CommandBase = command;
        
        // Compute hash of command (which includes all macros) but NOT the full file path
        // This avoids recompiling if just moving folders around
        result.HashHex = computeSha256Hex(hashCommand);
        
        // Build suggested output filename
        std::filesystem::path outFileName = options.SourceFilePath;
        outFileName.replace_extension(); // Remove .hlsl extension
        result.OutputFileNameNoExt = outFileName.filename().string() + "_" + result.HashHex;
        
        return result;
    }
    
    std::string appendOutputToCommand(
        const std::string& commandBase,
        const std::string& fullOutputPath,
        const std::string& pdbPath)
    {
        std::string command = commandBase;
        
        if (!pdbPath.empty())
            command += " /Fd \"" + pdbPath + "\"";
        
        command += " -Fo \"" + fullOutputPath + "\"";
        
        return command;
    }
    
    //////////////////////////////////////////////////////////////////////////
    // File timestamp utilities
    //////////////////////////////////////////////////////////////////////////
    
    bool isCompiledShaderUpToDate(
        const std::filesystem::path& compiledFile,
        std::filesystem::file_time_type lastSourceModified)
    {
        auto lastModifiedTime = GetFileModifiedTime(compiledFile);
        return lastModifiedTime.has_value() && (*lastModifiedTime) >= lastSourceModified;
    }

    caustica::ShaderKey makeShaderKey(
        const ShaderCompilerConfig& config,
        const DxcCommandOptions& options)
    {
        const std::filesystem::path logicalSource = options.LogicalSourceFileName.empty()
            ? options.SourceFilePath.filename()
            : options.LogicalSourceFileName;

        const caustica::ShaderKeyKind kind = IsLibraryProfile(options.Profile)
            ? caustica::ShaderKeyKind::ShaderLibrary
            : caustica::ShaderKeyKind::SingleShader;

        return caustica::makeShaderKey(
            logicalSource.generic_string(),
            options.EntryPoint,
            kind,
            caustica::shader::fromNvrhiGraphicsApi(config.GraphicsAPI),
            options.Profile,
            options.Macros);
    }
    
} // namespace ShaderCompilerUtils
