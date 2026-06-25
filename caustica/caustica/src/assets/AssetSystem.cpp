#include <assets/AssetSystem.h>
#include <assets/cache/TextureCache.h>
#include <core/log.h>

namespace caustica
{

static std::unique_ptr<AssetSystem> s_Instance;

// =============================================================================
// Singleton
// =============================================================================
AssetSystem& AssetSystem::Get()
{
    if (!s_Instance)
    {
        s_Instance.reset(new AssetSystem());
        s_Instance->m_Initialized = true;
    }
    return *s_Instance;
}

void AssetSystem::Initialize(std::shared_ptr<TextureCache> textureCache)
{
    auto& sys = Get();
    sys.m_TextureCache = std::move(textureCache);
    caustica::info("AssetSystem initialized");
}

void AssetSystem::Shutdown()
{
    if (s_Instance)
    {
        s_Instance->m_FileWatcher.Clear();
        s_Instance->m_TextureCache.reset();
        s_Instance.reset();
    }
}

// =============================================================================
// Update
// =============================================================================
void AssetSystem::Update(uint64_t frameIndex)
{
    (void)frameIndex;
    if (m_HotReloadEnabled)
        m_FileWatcher.Update();
}

// =============================================================================
// Hot Reload
// =============================================================================
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

            // Find the asset by path
            AssetId id = m_Registry.FindByPath(event.path);
            if (id.IsValid())
            {
                m_Registry.IncrementVersion(id);
                m_Registry.NotifyChanged(id);
            }
        });
}

// =============================================================================
// RegisterTexture
// =============================================================================
AssetId AssetSystem::RegisterTexture(const std::filesystem::path& path)
{
    return m_Registry.Register(path, AssetType::Texture);
}

} // namespace caustica
