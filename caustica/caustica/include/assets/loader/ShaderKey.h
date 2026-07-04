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

// Identifies a single compiled shader or shader library across all compiler backends.
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

    // When set, used for on-disk / pack cache file names instead of computeHashHex().
    // Path-tracing libraries keep the legacy DXC command hash here for compatibility.
    std::string                             cacheHashHex;

    [[nodiscard]] std::vector<ShaderMacro>  canonicalMacros() const;
    [[nodiscard]] std::string               canonicalMacroString() const;
    [[nodiscard]] std::string               computeHashHex() const;
    [[nodiscard]] std::string               effectiveCacheHashHex() const;
    [[nodiscard]] static std::string        FormatCacheFileNameNoExt(std::string_view hashHex);
    [[nodiscard]] std::string               cacheFileNameNoExt() const;
    [[nodiscard]] std::filesystem::path       cacheFilePath(const std::filesystem::path& binariesRoot) const;
    [[nodiscard]] std::string               packVfsPath(std::string_view packRoot = "/ShaderDynamic/Bin") const;

    [[nodiscard]] bool operator==(const ShaderKey& other) const;
};

[[nodiscard]] std::string CanonicalMacroString(const std::vector<ShaderMacro>& macros);

[[nodiscard]] ShaderKey MakeShaderKey(
    std::string_view sourcePath,
    std::string_view entryPoint,
    ShaderKeyKind kind,
    shader::Backend backend,
    ShaderCompilerUtils::ShaderProfile profile,
    const std::vector<ShaderMacro>& macros);

[[nodiscard]] ShaderKey MakeShaderLibraryKey(
    std::string_view sourcePath,
    shader::Backend backend,
    const std::vector<ShaderMacro>& macros,
    ShaderCompilerUtils::ShaderProfile profile = ShaderCompilerUtils::ShaderProfile::Library_6_6);

} // namespace caustica
