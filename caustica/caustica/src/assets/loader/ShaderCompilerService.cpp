#include <assets/loader/ShaderCompilerService.h>

#include <core/log.h>
#include <core/string_utils.h>
#include <core/vfs/VFS.h>

#include <cstring>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace caustica::shader
{

ShaderCompilerService::ShaderCompilerService(Config config)
    : m_config(std::move(config))
{
}

namespace
{
constexpr std::array<char, 8> c_ManifestMagic = { 'C', 'A', 'U', 'S', 'S', 'M', 'F', '1' };

std::string bytesToHex(const uint8_t* data, size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t index = 0; index < size; ++index)
    {
        result[index * 2] = digits[data[index] >> 4];
        result[index * 2 + 1] = digits[data[index] & 0xf];
    }
    return result;
}
}

std::string ShaderCompilerService::resolveLogicalShaderId(const char* fileName, const char* entryName) const
{
    if (!entryName)
        entryName = "main";

    std::string adjustedName = fileName;
    if (const size_t pos = adjustedName.find(".hlsl"); pos != std::string::npos)
        adjustedName.erase(pos, 5);

    if (entryName && std::strcmp(entryName, "main") != 0)
        adjustedName += "_" + std::string(entryName);

    std::replace(adjustedName.begin(), adjustedName.end(), '\\', '/');
    const bool isCausticaShader = caustica::string_utils::starts_with(adjustedName, "caustica/shaders");
    std::string logicalId = isCausticaShader ? "caustica/" + adjustedName : adjustedName;
    std::transform(logicalId.begin(), logicalId.end(), logicalId.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return logicalId;
}

bool ShaderCompilerService::ensureManifestLoaded()
{
    if (m_manifestLoaded)
        return !m_manifest.empty();
    m_manifestLoaded = true;

    if (!m_config.fileSystem)
        return false;
    const std::filesystem::path manifestPath = m_config.shaderBinBasePath / "manifest.bin";
    const std::shared_ptr<IBlob> blob = m_config.fileSystem->readFile(manifestPath);
    if (!blob || blob->size() < 12)
    {
        caustica::error("Couldn't read shader manifest from %s", manifestPath.generic_string().c_str());
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(blob->data());
    if (std::memcmp(bytes, c_ManifestMagic.data(), c_ManifestMagic.size()) != 0)
    {
        caustica::error("Shader manifest at %s has an unsupported format", manifestPath.generic_string().c_str());
        return false;
    }
    uint32_t entryCount = 0;
    std::memcpy(&entryCount, bytes + 8, sizeof(entryCount));
    constexpr size_t headerSize = 12;
    constexpr size_t entrySize = 64;
    if (entryCount > (blob->size() - headerSize) / entrySize)
    {
        caustica::error("Shader manifest at %s is truncated", manifestPath.generic_string().c_str());
        return false;
    }

    m_manifest.reserve(entryCount);
    for (uint32_t index = 0; index < entryCount; ++index)
    {
        const uint8_t* entry = bytes + headerSize + size_t(index) * entrySize;
        m_manifest.emplace(bytesToHex(entry, 32), bytesToHex(entry + 32, 32));
    }
    return true;
}

std::shared_ptr<IBlob> ShaderCompilerService::loadPrecompiledBytecode(const char* fileName, const char* entryName)
{
    if (!m_config.fileSystem)
        return nullptr;

    if (!ensureManifestLoaded())
        return nullptr;

    const std::string logicalId = resolveLogicalShaderId(fileName, entryName);
    const std::string stableId = ShaderCompilerUtils::computeSha256Hex("caustica-shader-id-v1|" + logicalId);
    const auto manifestIt = m_manifest.find(stableId);
    if (manifestIt == m_manifest.end())
    {
        caustica::error("Shader ID '%s' is missing from the ShaderBin manifest", logicalId.c_str());
        return nullptr;
    }

    const std::string& objectHash = manifestIt->second;
    const std::filesystem::path shaderFilePath = m_config.shaderBinBasePath
        / objectHash.substr(0, 2) / (objectHash.substr(2) + ".bin");
    std::shared_ptr<IBlob>& data = m_bytecodeCache[shaderFilePath.generic_string()];
    if (data)
        return data;

    data = m_config.fileSystem->readFile(shaderFilePath);
    if (!data)
    {
        caustica::error("Couldn't read the binary file for shader %s from %s",
            fileName,
            shaderFilePath.generic_string().c_str());
        return nullptr;
    }

    return data;
}

std::shared_ptr<IBlob> ShaderCompilerService::loadBytecodeForKey(const ShaderKey& key, std::string_view shaderBinRoot)
{
    if (!m_config.fileSystem)
        return nullptr;

    const std::filesystem::path shaderFilePath =
        std::filesystem::path(shaderBinRoot) / (key.cacheFileNameNoExt() + ".bin");
    std::shared_ptr<IBlob>& data = m_bytecodeCache[shaderFilePath.generic_string()];
    if (data)
        return data;

    data = m_config.fileSystem->readFile(shaderFilePath);
    return data;
}

void ShaderCompilerService::clearBytecodeCache()
{
    m_bytecodeCache.clear();
}

void ShaderCompilerService::forEachCachedBytecode(const std::function<void(const std::shared_ptr<IBlob>&)>& visitor) const
{
    if (!visitor)
        return;
    for (const auto& entry : m_bytecodeCache)
        visitor(entry.second);
}

} // namespace caustica::shader
