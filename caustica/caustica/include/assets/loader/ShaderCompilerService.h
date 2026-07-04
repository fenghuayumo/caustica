#pragma once

#include <assets/loader/ShaderKey.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace caustica
{
class IBlob;
class IFileSystem;
}

namespace caustica::shader
{

// Unified bytecode load/cache layer for static (ShaderMake) and dynamic (DXC) shaders.
class ShaderCompilerService
{
public:
    struct Config
    {
        std::shared_ptr<IFileSystem> fileSystem;
        std::filesystem::path precompiledBasePath = "/ShaderPrecompiled";
    };

    explicit ShaderCompilerService(Config config);

    [[nodiscard]] std::shared_ptr<IBlob> loadPrecompiledBytecode(const char* fileName, const char* entryName);
    [[nodiscard]] std::shared_ptr<IBlob> loadBytecodeForKey(const ShaderKey& key, std::string_view dynamicPackRoot = "/ShaderDynamic/Bin");

    void clearBytecodeCache();

    void forEachCachedBytecode(const std::function<void(const std::shared_ptr<IBlob>&)>& visitor) const;

    [[nodiscard]] const Config& config() const { return m_config; }

private:
    [[nodiscard]] std::filesystem::path resolvePrecompiledPath(const char* fileName, const char* entryName) const;

    Config m_config;
    std::unordered_map<std::string, std::shared_ptr<IBlob>> m_bytecodeCache;
};

} // namespace caustica::shader
