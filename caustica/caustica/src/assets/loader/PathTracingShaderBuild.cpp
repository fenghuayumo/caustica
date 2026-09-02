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
    // SPIR-V embeds -Zi inline (~70% of each library). DXIL writes external
    // PDBs, which are useful for development but must not change the cache key
    // of a distribution build: those builds ship the offline no-debug bins.
    options.EnableDebugInfo = config.GraphicsAPI != caustica::rhi::GraphicsAPI::VULKAN
        && CAUSTICA_DISTRIBUTION_BUILD == 0;
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
