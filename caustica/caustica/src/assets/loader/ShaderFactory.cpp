#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderCompilerService.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <core/string_utils.h>
#include <ShaderMake/ShaderBlob.h>
#if CAUSTICA_WITH_AFTERMATH
#include <backend/AftermathCrashDump.h>
#endif

using namespace std;
using namespace caustica;
using namespace caustica;

ShaderFactory::ShaderFactory(caustica::rhi::DeviceHandle rendererInterface,
	std::shared_ptr<IFileSystem> fs,
	const std::filesystem::path& basePath)
	: m_Device(rendererInterface)
	, m_compilerService(std::make_shared<shader::ShaderCompilerService>(shader::ShaderCompilerService::Config{
		.fileSystem = fs,
		.precompiledBasePath = basePath,
	}))
	, m_fs(fs)
	, m_basePath(basePath)
{
#if CAUSTICA_WITH_AFTERMATH
    if (m_Device->isAftermathEnabled())
        m_Device->getAftermathCrashDumpHelper().registerShaderBinaryLookupCallback(this, std::bind(&ShaderFactory::findShaderFromHash, this, std::placeholders::_1, std::placeholders::_2));
#endif
}

ShaderFactory::~ShaderFactory()
{
#if CAUSTICA_WITH_AFTERMATH
    if (m_Device->isAftermathEnabled())
        m_Device->getAftermathCrashDumpHelper().unRegisterShaderBinaryLookupCallback(this);
#endif
}

void ShaderFactory::clearCache()
{
	m_compilerService->clearBytecodeCache();
}

std::shared_ptr<IBlob> ShaderFactory::getBytecode(const char* fileName, const char* entryName)
{
    return m_compilerService->loadPrecompiledBytecode(fileName, entryName);
}

caustica::rhi::ShaderHandle ShaderFactory::createShader(const char* fileName, const char* entryName, const vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc)
{
    std::shared_ptr<IBlob> byteCode = getBytecode(fileName, entryName);

    if(!byteCode)
        return nullptr;

    caustica::rhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;
    if (descCopy.debugName.empty())
        descCopy.debugName = fileName;

    return createStaticShader(StaticShader{ byteCode->data(), byteCode->size() }, pDefines, descCopy);
}

caustica::rhi::ShaderHandle ShaderFactory::createShader(const char* fileName, const char* entryName, const vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType)
{
    return createShader(fileName, entryName, pDefines, caustica::rhi::ShaderDesc().setShaderType(shaderType));
}

caustica::rhi::ShaderLibraryHandle ShaderFactory::createShaderLibrary(const char* fileName, const std::vector<ShaderMacro>* pDefines)
{
    std::shared_ptr<IBlob> byteCode = getBytecode(fileName, nullptr);

    if (!byteCode)
        return nullptr;

    return createStaticShaderLibrary(StaticShader{ byteCode->data(), byteCode->size() }, pDefines);
}

caustica::rhi::ShaderHandle ShaderFactory::createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc)
{
    if (!shader.pBytecode || !shader.size)
        return nullptr;

    vector<ShaderMake::ShaderConstant> constants;
    if (pDefines)
    {
        for (const ShaderMacro& define : *pDefines)
            constants.push_back(ShaderMake::ShaderConstant{ define.name.c_str(), define.definition.c_str() });
    }

    const void* permutationBytecode = nullptr;
    size_t permutationSize = 0;
    if (!ShaderMake::FindPermutationInBlob(shader.pBytecode, shader.size, constants.data(), uint32_t(constants.size()), &permutationBytecode, &permutationSize))
    {
        const std::string message = ShaderMake::FormatShaderNotFoundMessage(shader.pBytecode, shader.size, constants.data(), uint32_t(constants.size()));
        caustica::error("%s", message.c_str());
        
        return nullptr;
    }

    return m_Device->createShader(desc, permutationBytecode, permutationSize);
}

caustica::rhi::ShaderHandle ShaderFactory::createStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType)
{
    return createStaticShader(shader, pDefines, caustica::rhi::ShaderDesc().setShaderType(shaderType));
}

caustica::rhi::ShaderHandle ShaderFactory::createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc)
{
    StaticShader shader;
    switch(m_Device->getGraphicsAPI())
    {
        case caustica::rhi::GraphicsAPI::D3D11:
            shader = dxbc;
            break;
        case caustica::rhi::GraphicsAPI::D3D12:
            shader = dxil;
            break;
        case caustica::rhi::GraphicsAPI::VULKAN:
            shader = spirv;
            break;
    }

    return createStaticShader(shader, pDefines, desc);
}

caustica::rhi::ShaderHandle ShaderFactory::createStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType)
{
    return createStaticPlatformShader(dxbc, dxil, spirv, pDefines, caustica::rhi::ShaderDesc().setShaderType(shaderType));
}

caustica::rhi::ShaderLibraryHandle ShaderFactory::createStaticShaderLibrary(StaticShader shader, const std::vector<ShaderMacro>* pDefines)
{
    if (!shader.pBytecode || !shader.size)
        return nullptr;

    vector<ShaderMake::ShaderConstant> constants;
    if (pDefines)
    {
        for (const ShaderMacro& define : *pDefines)
            constants.push_back(ShaderMake::ShaderConstant{ define.name.c_str(), define.definition.c_str() });
    }
    
    const void* permutationBytecode = nullptr;
    size_t permutationSize = 0;
    if (!ShaderMake::FindPermutationInBlob(shader.pBytecode, shader.size, constants.data(), uint32_t(constants.size()), &permutationBytecode, &permutationSize))
    {
        const std::string message = ShaderMake::FormatShaderNotFoundMessage(shader.pBytecode, shader.size, constants.data(), uint32_t(constants.size()));
        caustica::error("%s", message.c_str());

        return nullptr;
    }

    return m_Device->createShaderLibrary(permutationBytecode, permutationSize);
}

caustica::rhi::ShaderLibraryHandle ShaderFactory::createStaticPlatformShaderLibrary(StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines)
{
    StaticShader shader;
    switch(m_Device->getGraphicsAPI())
    {
        case caustica::rhi::GraphicsAPI::D3D12:
            shader = dxil;
            break;
        case caustica::rhi::GraphicsAPI::VULKAN:
            shader = spirv;
            break;
        default:
            break;
    }

    return createStaticShaderLibrary(shader, pDefines);
}

caustica::rhi::ShaderHandle ShaderFactory::createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const caustica::rhi::ShaderDesc& desc)
{
    caustica::rhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;
    if (descCopy.debugName.empty())
        descCopy.debugName = fileName;

    caustica::rhi::ShaderHandle shader = createStaticPlatformShader(dxbc, dxil, spirv, pDefines, descCopy);
    if (shader)
        return shader;
        
    return createShader(fileName, entryName, pDefines, desc);
}

caustica::rhi::ShaderHandle ShaderFactory::createAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, caustica::rhi::ShaderType shaderType)
{
    return createAutoShader(fileName, entryName, dxbc, dxil, spirv, pDefines, caustica::rhi::ShaderDesc().setShaderType(shaderType));
}

caustica::rhi::ShaderLibraryHandle ShaderFactory::createAutoShaderLibrary(const char* fileName, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines)
{
    caustica::rhi::ShaderLibraryHandle shader = createStaticPlatformShaderLibrary(dxil, spirv, pDefines);
    if (shader)
        return shader;

    return createShaderLibrary(fileName, pDefines);
}

std::pair<const void*, size_t> caustica::ShaderFactory::findShaderFromHash(uint64_t hash, std::function<uint64_t(std::pair<const void*, size_t>, caustica::rhi::GraphicsAPI)> hashGenerator)
{
    std::pair<const void*, size_t> result{ nullptr, 0 };
    m_compilerService->forEachCachedBytecode([&](const std::shared_ptr<IBlob>& blob) {
        if (result.first || !blob)
            return;

        const void* shaderBytes = blob->data();
        size_t shaderSize = blob->size();

        // the bytecode could contain multiple permutations
        std::vector<std::string> permutations;
        ShaderMake::EnumeratePermutationsInBlob(shaderBytes, shaderSize, permutations);

        if (permutations.size() > 1)
        {
            std::vector<ShaderMake::ShaderConstant> permutationConstants;
            std::unordered_map<std::string, std::string> permutationDefines;
            for (auto permutation : permutations)
            {
                permutationConstants.clear();
                permutationDefines.clear();
                // split the string by spaces to get individual defines
                std::vector<std::string> permutationStrings = caustica::string_utils::split(permutation, " ");
                for (auto& s : permutationStrings)
                {
                    std::vector<std::string> keyValue = caustica::string_utils::split(s, "=");
                    permutationDefines[keyValue[0]] = keyValue[1];
                }
                // now that we have processed all defines in this permutation, can create the shader constants
                for (const auto& [key, value] : permutationDefines)
                {
                    permutationConstants.push_back(ShaderMake::ShaderConstant{ key.c_str(), value.c_str() });
                }
                const void* permutationBytecode = nullptr;
                size_t permutationSize = 0;
                if (ShaderMake::FindPermutationInBlob(shaderBytes, shaderSize, permutationConstants.data(),
                    uint32_t(permutationConstants.size()), &permutationBytecode, &permutationSize))
                {
                    uint64_t entryHash = hashGenerator(std::make_pair(permutationBytecode, permutationSize), m_Device->getGraphicsAPI());
                    if (entryHash == hash)
                    {
                        result = std::make_pair(permutationBytecode, permutationSize);
                        return;
                    }
                }
            }
        }
        else
        {
            uint64_t entryHash = hashGenerator(std::make_pair(shaderBytes, shaderSize), m_Device->getGraphicsAPI());
            if (entryHash == hash)
            {
                result = std::make_pair(shaderBytes, shaderSize);
                return;
            }
        }
    });
    return result;
}
