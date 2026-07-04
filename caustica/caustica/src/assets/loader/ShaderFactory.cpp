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

ShaderFactory::ShaderFactory(nvrhi::DeviceHandle rendererInterface,
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
        m_Device->getAftermathCrashDumpHelper().registerShaderBinaryLookupCallback(this, std::bind(&ShaderFactory::FindShaderFromHash, this, std::placeholders::_1, std::placeholders::_2));
#endif
}

ShaderFactory::~ShaderFactory()
{
#if CAUSTICA_WITH_AFTERMATH
    if (m_Device->isAftermathEnabled())
        m_Device->getAftermathCrashDumpHelper().unRegisterShaderBinaryLookupCallback(this);
#endif
}

void ShaderFactory::ClearCache()
{
	m_compilerService->clearBytecodeCache();
}

std::shared_ptr<IBlob> ShaderFactory::GetBytecode(const char* fileName, const char* entryName)
{
    return m_compilerService->loadPrecompiledBytecode(fileName, entryName);
}

nvrhi::ShaderHandle ShaderFactory::CreateShader(const char* fileName, const char* entryName, const vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc)
{
    std::shared_ptr<IBlob> byteCode = GetBytecode(fileName, entryName);

    if(!byteCode)
        return nullptr;

    nvrhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;
    if (descCopy.debugName.empty())
        descCopy.debugName = fileName;

    return CreateStaticShader(StaticShader{ byteCode->data(), byteCode->size() }, pDefines, descCopy);
}

nvrhi::ShaderHandle ShaderFactory::CreateShader(const char* fileName, const char* entryName, const vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType)
{
    return CreateShader(fileName, entryName, pDefines, nvrhi::ShaderDesc().setShaderType(shaderType));
}

nvrhi::ShaderLibraryHandle ShaderFactory::CreateShaderLibrary(const char* fileName, const std::vector<ShaderMacro>* pDefines)
{
    std::shared_ptr<IBlob> byteCode = GetBytecode(fileName, nullptr);

    if (!byteCode)
        return nullptr;

    return CreateStaticShaderLibrary(StaticShader{ byteCode->data(), byteCode->size() }, pDefines);
}

nvrhi::ShaderHandle ShaderFactory::CreateStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc)
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

nvrhi::ShaderHandle ShaderFactory::CreateStaticShader(StaticShader shader, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType)
{
    return CreateStaticShader(shader, pDefines, nvrhi::ShaderDesc().setShaderType(shaderType));
}

nvrhi::ShaderHandle ShaderFactory::CreateStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc)
{
    StaticShader shader;
    switch(m_Device->getGraphicsAPI())
    {
        case nvrhi::GraphicsAPI::D3D11:
            shader = dxbc;
            break;
        case nvrhi::GraphicsAPI::D3D12:
            shader = dxil;
            break;
        case nvrhi::GraphicsAPI::VULKAN:
            shader = spirv;
            break;
    }

    return CreateStaticShader(shader, pDefines, desc);
}

nvrhi::ShaderHandle ShaderFactory::CreateStaticPlatformShader(StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType)
{
    return CreateStaticPlatformShader(dxbc, dxil, spirv, pDefines, nvrhi::ShaderDesc().setShaderType(shaderType));
}

nvrhi::ShaderLibraryHandle ShaderFactory::CreateStaticShaderLibrary(StaticShader shader, const std::vector<ShaderMacro>* pDefines)
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

nvrhi::ShaderLibraryHandle ShaderFactory::CreateStaticPlatformShaderLibrary(StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines)
{
    StaticShader shader;
    switch(m_Device->getGraphicsAPI())
    {
        case nvrhi::GraphicsAPI::D3D12:
            shader = dxil;
            break;
        case nvrhi::GraphicsAPI::VULKAN:
            shader = spirv;
            break;
        default:
            break;
    }

    return CreateStaticShaderLibrary(shader, pDefines);
}

nvrhi::ShaderHandle ShaderFactory::CreateAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, const nvrhi::ShaderDesc& desc)
{
    nvrhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;
    if (descCopy.debugName.empty())
        descCopy.debugName = fileName;

    nvrhi::ShaderHandle shader = CreateStaticPlatformShader(dxbc, dxil, spirv, pDefines, descCopy);
    if (shader)
        return shader;
        
    return CreateShader(fileName, entryName, pDefines, desc);
}

nvrhi::ShaderHandle ShaderFactory::CreateAutoShader(const char* fileName, const char* entryName, StaticShader dxbc, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines, nvrhi::ShaderType shaderType)
{
    return CreateAutoShader(fileName, entryName, dxbc, dxil, spirv, pDefines, nvrhi::ShaderDesc().setShaderType(shaderType));
}

nvrhi::ShaderLibraryHandle ShaderFactory::CreateAutoShaderLibrary(const char* fileName, StaticShader dxil, StaticShader spirv, const std::vector<ShaderMacro>* pDefines)
{
    nvrhi::ShaderLibraryHandle shader = CreateStaticPlatformShaderLibrary(dxil, spirv, pDefines);
    if (shader)
        return shader;

    return CreateShaderLibrary(fileName, pDefines);
}

std::pair<const void*, size_t> caustica::ShaderFactory::FindShaderFromHash(uint64_t hash, std::function<uint64_t(std::pair<const void*, size_t>, nvrhi::GraphicsAPI)> hashGenerator)
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
