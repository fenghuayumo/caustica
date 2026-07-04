#include <assets/loader/ShaderCompilerService.h>

#include <core/log.h>
#include <core/string_utils.h>
#include <core/vfs/VFS.h>

#include <cstring>
#include <functional>
#include <string>

namespace caustica::shader
{

ShaderCompilerService::ShaderCompilerService(Config config)
    : m_config(std::move(config))
{
}

std::filesystem::path ShaderCompilerService::resolvePrecompiledPath(const char* fileName, const char* entryName) const
{
    if (!entryName)
        entryName = "main";

    std::string adjustedName = fileName;
    if (const size_t pos = adjustedName.find(".hlsl"); pos != std::string::npos)
        adjustedName.erase(pos, 5);

    if (entryName && std::strcmp(entryName, "main") != 0)
        adjustedName += "_" + std::string(entryName);

    const bool isCausticaShader = caustica::string_utils::starts_with(adjustedName, "caustica/shaders");
    return isCausticaShader
        ? m_config.precompiledBasePath / "caustica" / (adjustedName + ".bin")
        : m_config.precompiledBasePath / (adjustedName + ".bin");
}

std::shared_ptr<IBlob> ShaderCompilerService::loadPrecompiledBytecode(const char* fileName, const char* entryName)
{
    if (!m_config.fileSystem)
        return nullptr;

    const std::filesystem::path shaderFilePath = resolvePrecompiledPath(fileName, entryName);
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

std::shared_ptr<IBlob> ShaderCompilerService::loadBytecodeForKey(const ShaderKey& key, std::string_view dynamicPackRoot)
{
    if (!m_config.fileSystem)
        return nullptr;

    const std::filesystem::path shaderFilePath =
        std::filesystem::path(dynamicPackRoot) / (key.cacheFileNameNoExt() + ".bin");
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
