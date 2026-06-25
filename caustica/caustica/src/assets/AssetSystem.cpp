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

void AssetSystem::Initialize(std::shared_ptr<TextureLoader> legacyTextureLoader)
{
    auto& sys = Get();
    sys.m_LegacyTextureLoader = std::move(legacyTextureLoader);
    caustica::info("AssetSystem initialized");
}

void AssetSystem::Shutdown()
{
    if (s_Instance)
    {
        s_Instance->m_FileWatcher.Clear();
        s_Instance->m_TextureCache.Clear();
        s_Instance->m_LegacyTextureLoader.reset();
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
    auto cached = m_TextureCache.Get(id);
    if (cached)
        return cached;
    // Fall back to legacy cache
    if (m_LegacyTextureLoader)
        return m_LegacyTextureLoader->GetLoadedTexture(path);
    return nullptr;
}

void AssetSystem::EvictTexturesToBudget()
{
    m_TextureCache.EvictToBudget(m_TextureMemoryBudget);
}

} // namespace caustica

