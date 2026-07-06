#pragma once

#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderKey.h>

#include <filesystem>
#include <vector>

namespace caustica
{

struct PathTracingShaderBuildInput
{
    std::filesystem::path                   logicalSourcePath;
    std::filesystem::path                   absoluteSourcePath;
    std::vector<ShaderMacro>                macros;
    bool                                    useOptimizations = true;
    bool                                    embedPdbs = false;
};

struct PathTracingShaderBuildResult
{
    ShaderKey                               key;
    ShaderCompilerUtils::DxcCommandResult     dxc;
};

[[nodiscard]] PathTracingShaderBuildResult buildPathTracingLibraryShader(
    const ShaderCompilerUtils::ShaderCompilerConfig& config,
    const PathTracingShaderBuildInput& input);

[[nodiscard]] std::string makePathTracingShaderCompileCommand(
    const ShaderCompilerUtils::ShaderCompilerConfig& config,
    const PathTracingShaderBuildResult& buildResult,
    const std::filesystem::path& outputBinPath,
    const std::filesystem::path& outputPdbPath = {});

} // namespace caustica
