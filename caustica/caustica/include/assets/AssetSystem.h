#pragma once

#include <assets/AssetId.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetCache.h>
#include <assets/LoadedTexture.h>
#include <assets/TextureData.h>

#include <filesystem>
#include <memory>
#include <string>

namespace nvrhi { class IDevice; class ICommandList; }

namespace caustica
{

class TextureLoader;
class IFileSystem;
class IDescriptorTableManager;
class IBlob;
class ThreadPool;

namespace render { class RenderDevice; }

// Owns texture registry/cache and the TextureLoader that uses them.
class AssetSystem
{
public:
    static AssetSystem& get();
    static void initialize(
        nvrhi::IDevice* device,
        std::shared_ptr<IFileSystem> fileSystem,
        std::shared_ptr<IDescriptorTableManager> descriptorTable);
    static void shutdown();

    AssetRegistry& getRegistry() { return m_Registry; }
    const AssetRegistry& getRegistry() const { return m_Registry; }
    AssetCache<TextureData>& getTextureCache() { return m_TextureCache; }

    [[nodiscard]] std::shared_ptr<TextureLoader> getTextureLoader() { return m_TextureLoader; }
    [[nodiscard]] bool isInitialized() const { return m_Initialized; }

    std::shared_ptr<LoadedTexture> loadTextureFromFile(
        const std::filesystem::path& path,
        bool sRGB,
        render::RenderDevice* renderDevice,
        nvrhi::ICommandList* commandList);

    std::shared_ptr<LoadedTexture> loadTextureFromFileDeferred(
        const std::filesystem::path& path,
        bool sRGB);

    std::shared_ptr<LoadedTexture> loadTextureFromFileAsync(
        const std::filesystem::path& path,
        bool sRGB,
        ThreadPool& threadPool);

    std::shared_ptr<LoadedTexture> loadTextureFromMemory(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        render::RenderDevice* renderDevice,
        nvrhi::ICommandList* commandList);

    std::shared_ptr<LoadedTexture> loadTextureFromMemoryDeferred(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB);

    std::shared_ptr<LoadedTexture> loadTextureFromMemoryAsync(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        ThreadPool& threadPool);

    std::shared_ptr<TextureData> getLoadedTexture(const std::filesystem::path& path);
    bool unloadTexture(const std::shared_ptr<LoadedTexture>& texture);

    bool processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds);
    void loadingFinished();

private:
    AssetSystem() = default;

    AssetRegistry m_Registry;
    AssetCache<TextureData> m_TextureCache;
    std::shared_ptr<TextureLoader> m_TextureLoader;
    bool m_Initialized = false;
};

} // namespace caustica
