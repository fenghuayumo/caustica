#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <rhi/nvrhi.h>
#include <memory>
#include <filesystem>
#include <functional>

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
    struct ShaderMacro
    {
        std::string name;
        std::string definition;

        ShaderMacro(const std::string& _name, const std::string& _definition)
            : name(_name)
            , definition(_definition)
        { }

        bool operator == (const ShaderMacro& other) const { return name == other.name && definition == other.definition; }
    };

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

    // Macro to use with ShaderFactory::CreateStaticPlatformShader.
    // If there are symbols g_MyShader_dxbc, g_MyShader_dxil, g_MyShader_spirv - just use:
    //      CreateStaticPlatformShader(CAUSTICA_MAKE_PLATFORM_SHADER(g_MyShader), defines, shaderDesc);
    // and all available platforms will be resolved automatically.
    #define CAUSTICA_MAKE_PLATFORM_SHADER(basename) CAUSTICA_MAKE_DXBC_SHADER(basename##_dxbc), CAUSTICA_MAKE_DXIL_SHADER(basename##_dxil), CAUSTICA_MAKE_SPIRV_SHADER(basename##_spirv)

    // Similar to CAUSTICA_MAKE_PLATFORM_SHADER but for libraries - they are not available on DX11/DXBC.
    //      CreateStaticPlatformShaderLibrary(CAUSTICA_MAKE_PLATFORM_SHADER_LIBRARY(g_MyShaderLibrary), defines);
    #define CAUSTICA_MAKE_PLATFORM_SHADER_LIBRARY(basename) CAUSTICA_MAKE_DXIL_SHADER(basename##_dxil), CAUSTICA_MAKE_SPIRV_SHADER(basename##_spirv)

    class ShaderFactory
    {
    private:
        nvrhi::DeviceHandle m_Device;
        std::shared_ptr<shader::ShaderCompilerService> m_compilerService;
		std::shared_ptr<caustica::IFileSystem> m_fs;
		std::filesystem::path m_basePath;

    public:
        ShaderFactory(
            nvrhi::DeviceHandle device,
            std::shared_ptr<caustica::IFileSystem> fs,
			const std::filesystem::path& basePath);

        virtual ~ShaderFactory();

        void ClearCache();

        [[nodiscard]] shader::ShaderCompilerService& compilerService() { return *m_compilerService; }
        [[nodiscard]] const shader::ShaderCompilerService& compilerService() const { return *m_compilerService; }

        std::shared_ptr<caustica::IBlob> GetBytecode(const char* fileName, const char* entryName);

        // Creates a shader from binary file.
        nvrhi::ShaderHandle CreateShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);

        // A version of CreateShader that takes a ShaderType instead of a full ShaderDesc.
        nvrhi::ShaderHandle CreateShader(const char* fileName, const char* entryName, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);

        // Creates a shader library from binary file.
        nvrhi::ShaderLibraryHandle CreateShaderLibrary(const char* fileName, const std::vector<ShaderMacro>* pDefines);

        // Creates a shader from the bytecode array.
        nvrhi::ShaderHandle CreateStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);

        // A version of CreateStaticShader that takes a ShaderType instead of a full ShaderDesc.
        nvrhi::ShaderHandle CreateStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);

        // Creates a shader from one of the platform-speficic bytecode arrays, selecting it based on the device's graphics API.
        nvrhi::ShaderHandle CreateStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);

        // A version of CreateStaticPlatformShader that takes a ShaderType instead of a full ShaderDesc.
        nvrhi::ShaderHandle CreateStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);

        // Creates a shader library from the bytecode array.
        nvrhi::ShaderLibraryHandle CreateStaticShaderLibrary(StaticShader shader, const std::vector<ShaderMacro>* pDefines);

        // Creates a shader library from one of the platform-speficic bytecode arrays, selecting it based on the device's graphics API.
        nvrhi::ShaderLibraryHandle CreateStaticPlatformShaderLibrary(StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);
      
        // Tries to create a shader from one of the platform-specific bytecode arrays (calling CreateStaticPlatformShader).
        // If that fails (e.g. there is no static bytecode), creates a shader from the filesystem binary file (calling CreateShader).
        nvrhi::ShaderHandle CreateAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc);

        // A versoin of CreateAutoShader that takes a ShaderType instead of a full ShaderDesc.
        nvrhi::ShaderHandle CreateAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType);

        // Tries to create a shader library from one of the platform-specific bytecode arrays (calling CreateStaticPlatformShaderLibrary).
        // If that fails (e.g. there is no static bytecode), creates a shader library from the filesystem binary file (calling CreateShaderLibrary).
        nvrhi::ShaderLibraryHandle CreateAutoShaderLibrary(const char* fileName, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines);

        // Looks up a shader binary based on a provided hash and the function used to generate it
        std::pair<const void*, size_t> FindShaderFromHash(uint64_t hash, std::function<uint64_t(std::pair<const void*, size_t>, nvrhi::GraphicsAPI)> hashGenerator);
    };
}