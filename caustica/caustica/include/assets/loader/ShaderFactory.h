#pragma once

#include <assets/loader/ShaderMacro.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <functional>

#include <rhi/rhi.h>

namespace caustica
{
class IBlob;
class IFileSystem;
}

namespace caustica::shader
{
class ShaderCompilerService;
}

namespace caustica
{
struct StaticShader
{
    void const* pBytecode = nullptr;
    size_t size = 0;
};

#if CAUSTICA_WITH_DX11 && CAUSTICA_WITH_STATIC_SHADERS
#define CAUSTICA_MAKE_DXBC_SHADER(symbol) caustica::StaticShader{symbol,sizeof(symbol)}
#else
#define CAUSTICA_MAKE_DXBC_SHADER(symbol) caustica::StaticShader()
#endif

#if CAUSTICA_WITH_DX12 && CAUSTICA_WITH_STATIC_SHADERS
#define CAUSTICA_MAKE_DXIL_SHADER(symbol) caustica::StaticShader{symbol,sizeof(symbol)}
#else
#define CAUSTICA_MAKE_DXIL_SHADER(symbol) caustica::StaticShader()
#endif

#if CAUSTICA_WITH_VULKAN && CAUSTICA_WITH_STATIC_SHADERS
#define CAUSTICA_MAKE_SPIRV_SHADER(symbol) caustica::StaticShader{symbol,sizeof(symbol)}
#else
#define CAUSTICA_MAKE_SPIRV_SHADER(symbol) caustica::StaticShader()
#endif

#define CAUSTICA_MAKE_PLATFORM_SHADER(basename) CAUSTICA_MAKE_DXBC_SHADER(basename##_dxbc), CAUSTICA_MAKE_DXIL_SHADER(basename##_dxil), CAUSTICA_MAKE_SPIRV_SHADER(basename##_spirv)
#define CAUSTICA_MAKE_PLATFORM_SHADER_LIBRARY(basename) CAUSTICA_MAKE_DXIL_SHADER(basename##_dxil), CAUSTICA_MAKE_SPIRV_SHADER(basename##_spirv)

class ShaderFactory
{
private:
    caustica::rhi::DeviceHandle m_Device;
    std::shared_ptr<shader::ShaderCompilerService> m_compilerService;
    std::shared_ptr<IFileSystem> m_fs;
    std::filesystem::path m_basePath;

public:
    ShaderFactory(
        caustica::rhi::DeviceHandle device,
        std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& basePath);

    virtual ~ShaderFactory();

    void clearCache();

    [[nodiscard]] shader::ShaderCompilerService& compilerService() { return *m_compilerService; }
    [[nodiscard]] const shader::ShaderCompilerService& compilerService() const { return *m_compilerService; }

    std::shared_ptr<IBlob> getBytecode(const char* fileName, const char* entryName);

    caustica::rhi::ShaderHandle createShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc);
    caustica::rhi::ShaderHandle createShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType);
    caustica::rhi::ShaderLibraryHandle createShaderLibrary(const char* fileName, const std::vector<ShaderMacro>* pDefines);
    caustica::rhi::ShaderHandle createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc);
    caustica::rhi::ShaderHandle createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType);
    caustica::rhi::ShaderHandle createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc);
    caustica::rhi::ShaderHandle createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType);
    caustica::rhi::ShaderLibraryHandle createStaticShaderLibrary(StaticShader shader, const std::vector<ShaderMacro>* pDefines);
    caustica::rhi::ShaderLibraryHandle createStaticPlatformShaderLibrary(StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);
    caustica::rhi::ShaderHandle createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc);
    caustica::rhi::ShaderHandle createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType);
    caustica::rhi::ShaderLibraryHandle createAutoShaderLibrary(const char* fileName, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);

    std::pair<const void*, size_t> findShaderFromHash(uint64_t hash, std::function<uint64_t(std::pair<const void*, size_t>, caustica::rhi::GraphicsAPI)> hashGenerator);
};

} // namespace caustica
