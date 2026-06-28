#include <assets/AssetSystem.h>
#include <assets/loader/TextureLoader.h>
#include <core/log.h>

namespace caustica
{

static std::unique_ptr<AssetSystem> s_Instance;

AssetSystem& AssetSystem::Get()
{
    if (!s_Instance)
    {
        s_Instance.reset(new AssetSystem());
        s_Instance->m_Initialized = true;
    }
    return *s_Instance;
}

void AssetSystem::Initialize(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fileSystem,
    std::shared_ptr<IDescriptorTableManager> descriptorTable)
{
    auto& sys = Get();
    sys.m_TextureLoader = std::make_shared<TextureLoader>(
        device, std::move(fileSystem), std::move(descriptorTable));
    caustica::info("AssetSystem initialized");
}

void AssetSystem::Shutdown()
{
    if (s_Instance)
    {
        s_Instance->m_FileWatcher.Clear();
        s_Instance->m_TextureCache.Clear();
        s_Instance->m_TextureLoader.reset();
        s_Instance.reset();
    }
}

void AssetSystem::Update(uint64_t frameIndex)
{
    (void)frameIndex;
    if (m_HotReloadEnabled)
        m_FileWatcher.Update();
}

void AssetSystem::EnableHotReload(bool enable)
{
    m_HotReloadEnabled = enable;
    m_FileWatcher.SetEnabled(enable);
    if (enable)
        caustica::info("AssetSystem: hot reload enabled");
}

void AssetSystem::WatchAssetDirectory(const std::filesystem::path& path)
{
    m_FileWatcher.WatchDirectory(path, true,
        [this](const FileChangeEvent& event)
        {
            caustica::info("AssetSystem: file changed — %s", event.path.string().c_str());

            AssetId id = m_Registry.FindByPath(event.path);
            if (id.IsValid())
            {
                m_Registry.IncrementVersion(id);
                m_Registry.NotifyChanged(id);
                // Also evict from cache so it gets reloaded
                m_TextureCache.Evict(id);
            }
        });
}

AssetId AssetSystem::RegisterTexture(const std::filesystem::path& path)
{
    return m_Registry.Register(path, AssetType::Texture);
}

std::shared_ptr<TextureData> AssetSystem::FindTextureByPath(const std::filesystem::path& path)
{
    AssetId id = m_Registry.FindByPath(path);
    if (!id.IsValid())
        return nullptr;
    return m_TextureCache.Get(id);
}

void AssetSystem::EvictTexturesToBudget()
{
    m_TextureCache.EvictToBudget(m_TextureMemoryBudget);
}

AssetId AssetSystem::RegisterMesh(const std::filesystem::path& path)
{
    return m_Registry.Register(path, AssetType::Mesh);
}

std::shared_ptr<LoadedTexture> AssetSystem::LoadTexture(
    const std::filesystem::path& path,
    bool sRGB,
    CommonRenderPasses* passes,
    nvrhi::ICommandList* commandList)
{
    if (!m_TextureLoader)
        return nullptr;
    return m_TextureLoader->LoadTextureFromFile(path, sRGB, passes, commandList);
}

std::shared_ptr<LoadedTexture> AssetSystem::LoadTextureDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    if (!m_TextureLoader)
        return nullptr;
    return m_TextureLoader->LoadTextureFromFileDeferred(path, sRGB);
}

std::shared_ptr<LoadedTexture> AssetSystem::LoadTextureAsync(
    const std::filesystem::path& path,
    bool sRGB,
    ThreadPool& threadPool)
{
    if (!m_TextureLoader)
        return nullptr;
    return m_TextureLoader->LoadTextureFromFileAsync(path, sRGB, threadPool);
}

} // namespace caustica
