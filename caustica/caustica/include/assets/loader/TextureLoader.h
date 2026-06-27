#pragma once

#include <scene/SceneTypes.h>
#include <assets/AssetId.h>
#include <core/log.h>

#include <rhi/nvrhi.h>
#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <queue>

namespace caustica
{
    class IBlob;
    class IFileSystem;
    class AssetRegistry;
}

namespace caustica
{
    class CommonRenderPasses;
    class ThreadPool;

    struct TextureSubresourceData
    {
        size_t rowPitch = 0;
        size_t depthPitch = 0;
        ptrdiff_t dataOffset = 0;
        size_t dataSize = 0;
    };

    struct TextureData : public LoadedTexture
    {
        std::shared_ptr<caustica::IBlob> data;

        nvrhi::Format format = nvrhi::Format::UNKNOWN;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
        uint32_t arraySize = 1;
        uint32_t mipLevels = 1;
        nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown;
        bool isRenderTarget = false;
        bool forceSRGB = false;

        // ArraySlice -> MipLevel -> TextureSubresourceData
        std::vector<std::vector<TextureSubresourceData>> dataLayout;
    };

    class TextureLoader
    {
    protected:
        nvrhi::DeviceHandle m_Device;
        nvrhi::CommandListHandle m_CommandList;

        std::queue<std::shared_ptr<TextureData>> m_TexturesToFinalize;
        std::shared_ptr<DescriptorTableManager> m_DescriptorTable;
        std::mutex m_TexturesToFinalizeMutex;

        std::shared_ptr<caustica::IFileSystem> m_fs;

        uint32_t m_MaxTextureSize = 0;

        bool m_GenerateMipmaps = true;

        caustica::Severity m_InfoLogSeverity = caustica::Severity::Info;
        caustica::Severity m_ErrorLogSeverity = caustica::Severity::Warning;

        std::atomic<uint32_t> m_TexturesRequested = 0;
        std::atomic<uint32_t> m_TexturesLoaded = 0;
        uint32_t m_TexturesFinalized = 0;

        bool FindTextureInCache(const std::filesystem::path& path, std::shared_ptr<TextureData>& texture);
        std::shared_ptr<caustica::IBlob> ReadTextureFile(const std::filesystem::path& path) const;

        bool FillTextureData(
            const std::shared_ptr<caustica::IBlob>& fileData,
            const std::shared_ptr<TextureData>& texture,
            const std::string& extension,
            const std::string& mimeType) const;

        void FinalizeTexture(
            std::shared_ptr<TextureData> texture,
            CommonRenderPasses* passes,
            nvrhi::ICommandList* commandList);

        virtual void TextureLoaded(std::shared_ptr<TextureData> texture);
        virtual std::shared_ptr<TextureData> CreateTextureData();

        // Register a texture path with the AssetRegistry and set its AssetId
        void RegisterTextureAsset(const std::shared_ptr<TextureData>& texture);

    public:
        TextureLoader(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::IFileSystem> fs,
            std::shared_ptr<DescriptorTableManager> descriptorTable);
        virtual ~TextureLoader();

        // Release all cached textures
        void Reset();

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromFile(
            const std::filesystem::path& path,
            bool sRGB,
            CommonRenderPasses* passes,
            nvrhi::ICommandList* commandList);

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromFileDeferred(
            const std::filesystem::path& path,
            bool sRGB);

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromFileAsync(
            const std::filesystem::path& path,
            bool sRGB,
            ThreadPool& threadPool);

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromMemoryAsync(
            const std::shared_ptr<caustica::IBlob>& data,
            const std::string& name,
            const std::string& mimeType,
            bool sRGB,
            ThreadPool& threadPool);

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromMemory(
            const std::shared_ptr<caustica::IBlob>& data,
            const std::string& name,
            const std::string& mimeType,
            bool sRGB,
            CommonRenderPasses* passes,
            nvrhi::ICommandList* commandList);

        virtual std::shared_ptr<LoadedTexture> LoadTextureFromMemoryDeferred(
            const std::shared_ptr<caustica::IBlob>& data,
            const std::string& name,
            const std::string& mimeType,
            bool sRGB);

        bool IsTextureLoaded(const std::shared_ptr<LoadedTexture>& texture);
        bool IsTextureFinalized(const std::shared_ptr<LoadedTexture>& texture);
        bool UnloadTexture(const std::shared_ptr<LoadedTexture>& texture);

        bool ProcessRenderingThreadCommands(CommonRenderPasses& passes, float timeLimitMilliseconds);
        void LoadingFinished();
        void SetMaxTextureSize(uint32_t size);
        void SetGenerateMipmaps(bool generateMipmaps);
        void SetInfoLogSeverity(caustica::Severity value) { m_InfoLogSeverity = value; }
        void SetErrorLogSeverity(caustica::Severity value) { m_ErrorLogSeverity = value; }

        uint32_t GetNumberOfLoadedTextures() { return m_TexturesLoaded.load(); }
        uint32_t GetNumberOfRequestedTextures() { return m_TexturesRequested.load(); }
        uint32_t GetNumberOfFinalizedTextures() { return m_TexturesFinalized; }

        std::shared_ptr<TextureData> GetLoadedTexture(std::filesystem::path const& path);
    };

    // Saves the contents of texture's slice 0 mip level 0 into an image file.
    // The image format is determined from the file's extension.
    // Supported formats are: BMP, PNG, JPG, TGA.
    // Requires that no immediate command list is open at the time this function is called.
    // Creates and destroys temporary resources internally, so should NOT be called often.
    bool SaveTextureToFile(
        nvrhi::IDevice* device,
        CommonRenderPasses* pPasses,
        nvrhi::ITexture* texture,
        nvrhi::ResourceStates textureState,
        const char* fileName,
        bool saveAlphaChannel = true);
}
