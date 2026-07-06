#include <assets/AssetSystem.h>
#include <assets/loader/TextureLoader.h>
#include <core/ThreadPool.h>
#include <core/log.h>

namespace caustica
{

static std::unique_ptr<AssetSystem> s_Instance;

AssetSystem& AssetSystem::get()
{
    if (!s_Instance)
    {
        s_Instance.reset(new AssetSystem());
        s_Instance->m_Initialized = true;
    }
    return *s_Instance;
}

void AssetSystem::initialize(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fileSystem,
    std::shared_ptr<IDescriptorTableManager> descriptorTable)
{
    auto& sys = get();
    sys.m_TextureLoader = std::make_shared<TextureLoader>(
        device,
        std::move(fileSystem),
        std::move(descriptorTable),
        sys.m_Registry,
        sys.m_TextureCache);
    caustica::info("AssetSystem initialized");
}

void AssetSystem::shutdown()
{
    if (s_Instance)
    {
        s_Instance->m_TextureCache.clear();
        s_Instance->m_TextureLoader.reset();
        s_Instance.reset();
    }
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    return m_TextureLoader->loadTextureFromFile(path, sRGB, renderDevice, commandList);
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    return m_TextureLoader->loadTextureFromFileDeferred(path, sRGB);
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB,
    ThreadPool& threadPool)
{
    return m_TextureLoader->loadTextureFromFileAsync(path, sRGB, threadPool);
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromMemory(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    return m_TextureLoader->loadTextureFromMemory(data, name, mimeType, sRGB, renderDevice, commandList);
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromMemoryDeferred(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    return m_TextureLoader->loadTextureFromMemoryDeferred(data, name, mimeType, sRGB);
}

std::shared_ptr<LoadedTexture> AssetSystem::loadTextureFromMemoryAsync(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    ThreadPool& threadPool)
{
    return m_TextureLoader->loadTextureFromMemoryAsync(data, name, mimeType, sRGB, threadPool);
}

std::shared_ptr<TextureData> AssetSystem::getLoadedTexture(const std::filesystem::path& path)
{
    return m_TextureLoader->getLoadedTexture(path);
}

bool AssetSystem::unloadTexture(const std::shared_ptr<LoadedTexture>& texture)
{
    return m_TextureLoader->unloadTexture(texture);
}

bool AssetSystem::processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds)
{
    return m_TextureLoader->processRenderingThreadCommands(renderDevice, timeLimitMilliseconds);
}

void AssetSystem::loadingFinished()
{
    m_TextureLoader->loadingFinished();
}

} // namespace caustica
