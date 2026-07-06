#pragma once

#include <assets/TextureData.h>
#include <assets/AssetCache.h>
#include <assets/AssetRegistry.h>
#include <backend/IDescriptorTableManager.h>
#include <core/log.h>

#include <rhi/nvrhi.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>

namespace caustica
{
class IBlob;
class IFileSystem;
class ThreadPool;

namespace render { class RenderDevice; }

class TextureLoader
{
public:
    TextureLoader(
        nvrhi::IDevice* device,
        std::shared_ptr<IFileSystem> fs,
        std::shared_ptr<IDescriptorTableManager> descriptorTable,
        AssetRegistry& registry,
        AssetCache<TextureData>& cache);
    ~TextureLoader();

    void reset();

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

    std::shared_ptr<LoadedTexture> loadTextureFromMemoryAsync(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
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

    bool isTextureLoaded(const std::shared_ptr<LoadedTexture>& texture);
    bool isTextureFinalized(const std::shared_ptr<LoadedTexture>& texture);
    bool unloadTexture(const std::shared_ptr<LoadedTexture>& texture);

    bool processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds);
    void loadingFinished();

    void setMaxTextureSize(uint32_t size);
    void setGenerateMipmaps(bool generateMipmaps);
    void setInfoLogSeverity(Severity value) { m_InfoLogSeverity = value; }
    void setErrorLogSeverity(Severity value) { m_ErrorLogSeverity = value; }

    uint32_t getNumberOfLoadedTextures() { return m_TexturesLoaded.load(); }
    uint32_t getNumberOfRequestedTextures() { return m_TexturesRequested.load(); }
    uint32_t getNumberOfFinalizedTextures() { return m_TexturesFinalized; }

    std::shared_ptr<TextureData> getLoadedTexture(std::filesystem::path const& path);

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::CommandListHandle m_CommandList;

    std::queue<std::shared_ptr<TextureData>> m_TexturesToFinalize;
    std::shared_ptr<IDescriptorTableManager> m_DescriptorTable;
    std::mutex m_TexturesToFinalizeMutex;

    std::shared_ptr<IFileSystem> m_fs;
    AssetRegistry& m_Registry;
    AssetCache<TextureData>& m_Cache;

    uint32_t m_MaxTextureSize = 0;
    bool m_GenerateMipmaps = true;

    Severity m_InfoLogSeverity = Severity::Info;
    Severity m_ErrorLogSeverity = Severity::Warning;

    std::atomic<uint32_t> m_TexturesRequested = 0;
    std::atomic<uint32_t> m_TexturesLoaded = 0;
    uint32_t m_TexturesFinalized = 0;

    bool findTextureInCache(const std::filesystem::path& path, std::shared_ptr<TextureData>& texture);
    std::shared_ptr<IBlob> readTextureFile(const std::filesystem::path& path) const;

    bool fillTextureData(
        const std::shared_ptr<IBlob>& fileData,
        const std::shared_ptr<TextureData>& texture,
        const std::string& extension,
        const std::string& mimeType) const;

    void finalizeTexture(
        std::shared_ptr<TextureData> texture,
        render::RenderDevice* renderDevice,
        nvrhi::ICommandList* commandList);

    void textureLoaded(std::shared_ptr<TextureData> texture);
    std::shared_ptr<TextureData> createTextureData();
    void registerTextureAsset(const std::shared_ptr<TextureData>& texture);
};

bool saveTextureToFile(
    nvrhi::IDevice* device,
    render::RenderDevice& renderDevice,
    nvrhi::ITexture* texture,
    nvrhi::ResourceStates textureState,
    const char* fileName,
    bool saveAlphaChannel = true);

} // namespace caustica
