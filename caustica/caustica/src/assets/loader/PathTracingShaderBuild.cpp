#include <assets/loader/PathTracingShaderBuild.h>
#include <assets/loader/ShaderBackend.h>

namespace caustica
{

PathTracingShaderBuildResult buildPathTracingLibraryShader(
    const ShaderCompilerUtils::ShaderCompilerConfig& config,
    const PathTracingShaderBuildInput& input)
{
    ShaderCompilerUtils::DxcCommandOptions options{};
    options.SourceFilePath = input.absoluteSourcePath;
    options.LogicalSourceFileName = input.logicalSourcePath;
    options.Profile = ShaderCompilerUtils::ShaderProfile::Library_6_6;
    options.EnableDebugInfo = true;
    options.EmbedPdb = input.embedPdbs;
    options.UseOptimizations = input.useOptimizations;
    options.Enable16BitTypes = true;
    options.WarningsAsErrors = true;
    options.AllResourcesBound = true;
    options.EnableDebugPrint = true;
    options.Macros = input.macros;

    PathTracingShaderBuildResult result{};
    result.dxc = ShaderCompilerUtils::buildDxcCommand(config, options);
    result.key = makeShaderLibraryKey(
        input.logicalSourcePath.generic_string(),
        shader::fromRhiGraphicsApi(config.GraphicsAPI),
        input.macros,
        options.Profile);
    result.key.cacheHashHex = result.dxc.HashHex;
    return result;
}

std::string makePathTracingShaderCompileCommand(
    const ShaderCompilerUtils::ShaderCompilerConfig& config,
    const PathTracingShaderBuildResult& buildResult,
    const std::filesystem::path& outputBinPath,
    const std::filesystem::path& outputPdbPath)
{
    std::string command = config.getCompilerPathQuoted() + buildResult.dxc.CommandBase;
    return ShaderCompilerUtils::appendOutputToCommand(
        command,
        outputBinPath.string(),
        outputPdbPath.string());
}

} // namespace caustica
