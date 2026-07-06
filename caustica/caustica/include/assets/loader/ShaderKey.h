#pragma once

#include <assets/loader/ShaderCompilerUtils.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderBackend.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace caustica
{

enum class ShaderKeyKind : uint8_t
{
    SingleShader,
    ShaderLibrary,
};

struct ShaderKey
{
    std::string                             sourcePath;
    std::string                             entryPoint;
    ShaderKeyKind                           kind             = ShaderKeyKind::SingleShader;
    shader::Backend                         backend          = shader::Backend::D3D12;
    ShaderCompilerUtils::ShaderProfile      profile          = ShaderCompilerUtils::ShaderProfile::Library_6_6;
    std::vector<ShaderMacro>                macros;

    std::string                             cacheHashHex;

    [[nodiscard]] std::vector<ShaderMacro>  canonicalMacros() const;
    [[nodiscard]] std::string               canonicalMacroString() const;
    [[nodiscard]] std::string               computeHashHex() const;
    [[nodiscard]] std::string               effectiveCacheHashHex() const;
    [[nodiscard]] static std::string        formatCacheFileNameNoExt(std::string_view hashHex);
    [[nodiscard]] std::string               cacheFileNameNoExt() const;
    [[nodiscard]] std::filesystem::path       cacheFilePath(const std::filesystem::path& binariesRoot) const;
    [[nodiscard]] std::string               packVfsPath(std::string_view packRoot = "/ShaderDynamic/Bin") const;

    [[nodiscard]] bool operator==(const ShaderKey& other) const;
};

[[nodiscard]] std::string formatCanonicalMacros(const std::vector<ShaderMacro>& macros);

[[nodiscard]] ShaderKey makeShaderKey(
    std::string_view sourcePath,
    std::string_view entryPoint,
    ShaderKeyKind kind,
    shader::Backend backend,
    ShaderCompilerUtils::ShaderProfile profile,
    const std::vector<ShaderMacro>& macros);

[[nodiscard]] ShaderKey makeShaderLibraryKey(
    std::string_view sourcePath,
    shader::Backend backend,
    const std::vector<ShaderMacro>& macros,
    ShaderCompilerUtils::ShaderProfile profile = ShaderCompilerUtils::ShaderProfile::Library_6_6);

} // namespace caustica
