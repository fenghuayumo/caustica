#pragma once

#include <assets/AssetStore.h>
#include <assets/Handle.h>
#include <assets/ImageAsset.h>
#include <assets/AssetRegistry.h>
#include <backend/IDescriptorTableManager.h>
#include <core/log.h>

#include <rhi/rhi.h>
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
        caustica::rhi::IDevice* device,
        std::shared_ptr<IFileSystem> fs,
        std::shared_ptr<IDescriptorTableManager> descriptorTable,
        AssetRegistry& registry,
        AssetStore<ImageAsset>& images);
    ~TextureLoader();

    void reset();

    Handle<ImageAsset> loadTextureFromFile(
        const std::filesystem::path& path,
        bool sRGB,
        render::RenderDevice* renderDevice,
        caustica::rhi::ICommandList* commandList);

    Handle<ImageAsset> loadTextureFromFileDeferred(
        const std::filesystem::path& path,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromFileAsync(
        const std::filesystem::path& path,
        bool sRGB,
        ThreadPool& threadPool);

    Handle<ImageAsset> loadTextureFromMemoryAsync(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        ThreadPool& threadPool);

    Handle<ImageAsset> loadTextureFromMemory(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        render::RenderDevice* renderDevice,
        caustica::rhi::ICommandList* commandList);

    Handle<ImageAsset> loadTextureFromMemoryDeferred(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB);

    bool isTextureLoaded(const Handle<ImageAsset>& texture);
    bool isTextureFinalized(const Handle<ImageAsset>& texture);
    bool unloadTexture(const Handle<ImageAsset>& texture);

    bool processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds);
    void loadingFinished();

    void setMaxTextureSize(uint32_t size);
    void setGenerateMipmaps(bool generateMipmaps);
    void setInfoLogSeverity(Severity value) { m_InfoLogSeverity = value; }
    void setErrorLogSeverity(Severity value) { m_ErrorLogSeverity = value; }

    uint32_t getNumberOfLoadedTextures() { return m_TexturesLoaded.load(); }
    uint32_t getNumberOfRequestedTextures() { return m_TexturesRequested.load(); }
    uint32_t getNumberOfFinalizedTextures() { return m_TexturesFinalized; }

    std::shared_ptr<ImageAsset> getLoadedTexture(std::filesystem::path const& path);
    std::shared_ptr<ImageAsset> getImage(const Handle<ImageAsset>& image) const { return image.shared(); }

private:
    caustica::rhi::DeviceHandle m_Device;
    caustica::rhi::CommandListHandle m_CommandList;

    std::queue<std::shared_ptr<ImageAsset>> m_TexturesToFinalize;
    std::shared_ptr<IDescriptorTableManager> m_DescriptorTable;
    std::mutex m_TexturesToFinalizeMutex;

    std::shared_ptr<IFileSystem> m_fs;
    AssetRegistry& m_Registry;
    AssetStore<ImageAsset>& m_Images;

    uint32_t m_MaxTextureSize = 0;
    bool m_GenerateMipmaps = true;

    Severity m_InfoLogSeverity = Severity::Info;
    Severity m_ErrorLogSeverity = Severity::Warning;

    std::atomic<uint32_t> m_TexturesRequested = 0;
    std::atomic<uint32_t> m_TexturesLoaded = 0;
    uint32_t m_TexturesFinalized = 0;

    bool findTextureInCache(const std::filesystem::path& path, std::shared_ptr<ImageAsset>& texture);
    std::shared_ptr<IBlob> readTextureFile(const std::filesystem::path& path) const;

    bool fillTextureData(
        const std::shared_ptr<IBlob>& fileData,
        const std::shared_ptr<ImageAsset>& texture,
        const std::string& extension,
        const std::string& mimeType) const;

    void finalizeTexture(
        std::shared_ptr<ImageAsset> texture,
        render::RenderDevice* renderDevice,
        caustica::rhi::ICommandList* commandList);

    void textureLoaded(std::shared_ptr<ImageAsset> texture);
    std::shared_ptr<ImageAsset> createTextureData();
    void registerTextureAsset(const std::shared_ptr<ImageAsset>& texture);
    Handle<ImageAsset> makeHandle(const std::shared_ptr<ImageAsset>& texture) const;
};

bool saveTextureToFile(
    caustica::rhi::IDevice* device,
    render::RenderDevice& renderDevice,
    caustica::rhi::ITexture* texture,
    caustica::rhi::ResourceStates textureState,
    const char* fileName,
    bool saveAlphaChannel = true);

} // namespace caustica
