#include <assets/loader/ShaderKey.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace caustica
{

namespace
{
    std::string ProfileToken(ShaderCompilerUtils::ShaderProfile profile)
    {
        switch (profile)
        {
        case ShaderCompilerUtils::ShaderProfile::Library_6_6: return "lib_6_6";
        case ShaderCompilerUtils::ShaderProfile::Library_6_9: return "lib_6_9";
        case ShaderCompilerUtils::ShaderProfile::Compute_6_6: return "cs_6_6";
        case ShaderCompilerUtils::ShaderProfile::Compute_6_9: return "cs_6_9";
        default: return "unknown";
        }
    }

    std::string KindToken(ShaderKeyKind kind)
    {
        return kind == ShaderKeyKind::ShaderLibrary ? "library" : "shader";
    }
}

std::string formatCanonicalMacros(const std::vector<ShaderMacro>& macros)
{
    std::vector<ShaderMacro> sorted = macros;
    std::sort(sorted.begin(), sorted.end(), [](const ShaderMacro& a, const ShaderMacro& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.definition < b.definition;
    });

    std::ostringstream result;
    bool first = true;
    for (const ShaderMacro& macro : sorted)
    {
        if (!first)
            result << ',';
        first = false;
        result << macro.name << '=' << macro.definition;
    }
    return result.str();
}

std::vector<ShaderMacro> ShaderKey::canonicalMacros() const
{
    std::vector<ShaderMacro> sorted = macros;
    std::sort(sorted.begin(), sorted.end(), [](const ShaderMacro& a, const ShaderMacro& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.definition < b.definition;
    });
    return sorted;
}

std::string ShaderKey::canonicalMacroString() const
{
    return formatCanonicalMacros(macros);
}

std::string ShaderKey::computeHashHex() const
{
    std::ostringstream hashInput;
    hashInput << KindToken(kind) << '|'
              << shader::backendToken(backend) << '|'
              << ProfileToken(profile) << '|'
              << sourcePath << '|'
              << entryPoint << '|'
              << formatCanonicalMacros(macros);
    return ShaderCompilerUtils::computeSha256Hex(hashInput.str());
}

std::string ShaderKey::effectiveCacheHashHex() const
{
    return cacheHashHex.empty() ? computeHashHex() : cacheHashHex;
}

std::string ShaderKey::formatCacheFileNameNoExt(std::string_view hashHex)
{
    if (hashHex.size() >= 2)
        return std::string(hashHex.substr(0, 2)) + "/" + std::string(hashHex.substr(2));
    return std::string(hashHex);
}

std::string ShaderKey::cacheFileNameNoExt() const
{
    return formatCacheFileNameNoExt(effectiveCacheHashHex());
}

std::filesystem::path ShaderKey::cacheFilePath(const std::filesystem::path& binariesRoot) const
{
    // Build real path components — do not embed '/' in a single filename segment
    // (Windows path APIs are brittle with mixed separators from formatCacheFileNameNoExt).
    const std::string hashHex = effectiveCacheHashHex();
    if (hashHex.size() >= 2)
        return binariesRoot / hashHex.substr(0, 2) / (hashHex.substr(2) + ".bin");
    return binariesRoot / (hashHex + ".bin");
}

std::string ShaderKey::packVfsPath(std::string_view packRoot) const
{
    std::string root(packRoot);
    if (!root.empty() && root.back() != '/')
        root += '/';
    return root + cacheFileNameNoExt() + ".bin";
}

bool ShaderKey::operator==(const ShaderKey& other) const
{
    return kind == other.kind
        && backend == other.backend
        && profile == other.profile
        && sourcePath == other.sourcePath
        && entryPoint == other.entryPoint
        && canonicalMacroString() == other.canonicalMacroString();
}

ShaderKey makeShaderKey(
    std::string_view sourcePath,
    std::string_view entryPoint,
    ShaderKeyKind kind,
    shader::Backend backend,
    ShaderCompilerUtils::ShaderProfile profile,
    const std::vector<ShaderMacro>& macros)
{
    ShaderKey key{};
    key.sourcePath = std::string(sourcePath);
    key.entryPoint = std::string(entryPoint);
    key.kind = kind;
    key.backend = backend;
    key.profile = profile;
    key.macros = macros;
    return key;
}

ShaderKey makeShaderLibraryKey(
    std::string_view sourcePath,
    shader::Backend backend,
    const std::vector<ShaderMacro>& macros,
    ShaderCompilerUtils::ShaderProfile profile)
{
    return makeShaderKey(sourcePath, {}, ShaderKeyKind::ShaderLibrary, backend, profile, macros);
}

} // namespace caustica
