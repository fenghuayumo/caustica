#pragma once

#include <assets/loader/ShaderMacro.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <functional>

#include <rhi/nvrhi.h>

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
    nvrhi::DeviceHandle m_Device;
    std::shared_ptr<shader::ShaderCompilerService> m_compilerService;
    std::shared_ptr<IFileSystem> m_fs;
    std::filesystem::path m_basePath;

public:
    ShaderFactory(
        nvrhi::DeviceHandle device,
        std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& basePath);

    virtual ~ShaderFactory();

    void clearCache();

    [[nodiscard]] shader::ShaderCompilerService& compilerService() { return *m_compilerService; }
    [[nodiscard]] const shader::ShaderCompilerService& compilerService() const { return *m_compilerService; }

    std::shared_ptr<IBlob> getBytecode(const char* fileName, const char* entryName);

    nvrhi::ShaderHandle createShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);
    nvrhi::ShaderHandle createShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);
    nvrhi::ShaderLibraryHandle createShaderLibrary(const char* fileName, const std::vector<ShaderMacro>* pDefines);
    nvrhi::ShaderHandle createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);
    nvrhi::ShaderHandle createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);
    nvrhi::ShaderHandle createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);
    nvrhi::ShaderHandle createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);
    nvrhi::ShaderLibraryHandle createStaticShaderLibrary(StaticShader shader, const std::vector<ShaderMacro>* pDefines);
    nvrhi::ShaderLibraryHandle createStaticPlatformShaderLibrary(StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);
    nvrhi::ShaderHandle createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);
    nvrhi::ShaderHandle createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);
    nvrhi::ShaderLibraryHandle createAutoShaderLibrary(const char* fileName, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);

    std::pair<const void*, size_t> findShaderFromHash(uint64_t hash, std::function<uint64_t(std::pair<const void*, size_t>, nvrhi::GraphicsAPI)> hashGenerator);
};

} // namespace caustica
