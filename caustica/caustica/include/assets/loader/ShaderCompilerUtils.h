#pragma once

#include <assets/loader/ShaderMacro.h>
#include <assets/loader/ShaderFactory.h>

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <rhi/nvrhi.h>

namespace caustica { struct ShaderKey; }

namespace ShaderCompilerUtils
{

constexpr size_t k_Sha256DigestSize = 32;

std::string computeSha256Hex(const std::string& input);

struct ShaderCompilerConfig
{
    std::filesystem::path   ShaderCompilerPath;
    std::filesystem::path   ShadersPath;
    std::filesystem::path   ShadersPathExternalIncludes1;
    std::filesystem::path   ShadersPathExternalIncludes2;
    std::filesystem::path   ShaderBinariesPath;
    nvrhi::GraphicsAPI      GraphicsAPI;
    bool                    RuntimeCompilationAvailable = false;

    bool initialize(nvrhi::IDevice* device, const std::string& binarySubfolder);
    std::string getCompilerPathQuoted() const;
    bool canCompile() const { return RuntimeCompilationAvailable; }
};

enum class ShaderProfile
{
    Library_6_6,
    Library_6_9,
    Compute_6_6,
    Compute_6_9,
};

struct DxcCommandOptions
{
    std::filesystem::path   SourceFilePath;
    std::filesystem::path   LogicalSourceFileName;
    ShaderProfile           Profile = ShaderProfile::Compute_6_6;
    std::string             EntryPoint;
    bool                    EnableDebugInfo = true;
    bool                    EmbedPdb = false;
    bool                    UseOptimizations = true;
    bool                    Enable16BitTypes = true;
    bool                    WarningsAsErrors = true;
    bool                    AllResourcesBound = true;
    bool                    EnableDebugPrint = true;
    std::vector<caustica::ShaderMacro> Macros;
    std::vector<std::filesystem::path> AdditionalIncludes;
};

struct DxcCommandResult
{
    std::string CommandBase;
    std::string HashHex;
    std::string OutputFileNameNoExt;
};

DxcCommandResult buildDxcCommand(
    const ShaderCompilerConfig& config,
    const DxcCommandOptions& options);

std::string appendOutputToCommand(
    const std::string& commandBase,
    const std::string& fullOutputPath,
    const std::string& pdbPath = "");

bool isCompiledShaderUpToDate(
    const std::filesystem::path& compiledFile,
    std::filesystem::file_time_type lastSourceModified);

[[nodiscard]] caustica::ShaderKey makeShaderKey(
    const ShaderCompilerConfig& config,
    const DxcCommandOptions& options);

} // namespace ShaderCompilerUtils
