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
        std::filesystem::path shaderBinBasePath = "/ShaderBin";
    };

    explicit ShaderCompilerService(Config config);

    [[nodiscard]] std::shared_ptr<IBlob> loadPrecompiledBytecode(const char* fileName, const char* entryName);
    [[nodiscard]] std::shared_ptr<IBlob> loadBytecodeForKey(const ShaderKey& key, std::string_view shaderBinRoot = "/ShaderBin");

    void clearBytecodeCache();

    void forEachCachedBytecode(const std::function<void(const std::shared_ptr<IBlob>&)>& visitor) const;

    [[nodiscard]] const Config& config() const { return m_config; }

private:
    [[nodiscard]] std::string resolveLogicalShaderId(const char* fileName, const char* entryName) const;
    bool ensureManifestLoaded();

    Config m_config;
    std::unordered_map<std::string, std::shared_ptr<IBlob>> m_bytecodeCache;
    std::unordered_map<std::string, std::string> m_manifest;
    bool m_manifestLoaded = false;
};

} // namespace caustica::shader
