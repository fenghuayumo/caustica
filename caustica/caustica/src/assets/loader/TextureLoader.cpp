#include <assets/loader/TextureLoader.h>
#include <assets/AssetSystem.h>

#include <render/Core/DescriptorTableManager.h>
#include <render/Core/CommonRenderPasses.h>
#include <engine/ConsoleObjects.h>
#include <assets/loader/DDSFile.h>
#include <core/ThreadPool.h>
#include <core/vfs/VFS.h>
#include <core/log.h>

#include <stb_image.h>
#include <stb_image_write.h>

#ifdef CAUSTICA_WITH_TINYEXR

    #if defined (_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable:4018) // Silence warning from tinyEXR
    #endif

    #define TINYEXR_IMPLEMENTATION
    #include <tinyexr.h>

    #if defined (_MSC_VER)
        #pragma warning(pop)
    #endif

#endif // CAUSTICA_WITH_TINYEXR

#include <algorithm>
#include <chrono>
#include <regex>

using namespace caustica::math;
using namespace caustica;
using namespace caustica;

class StbImageBlob : public IBlob
{
private:
    unsigned char* m_data = nullptr;

public:
    StbImageBlob(unsigned char* _data) : m_data(_data) 
    {
    }

    virtual ~StbImageBlob()
    {
        if (m_data)
        {
            stbi_image_free(m_data);
            m_data = nullptr;
        }
    }

    virtual const void* data() const override
    {
        return m_data;
    }

    virtual size_t size() const override
    {
        return 0; // nobody cares
    }
};


TextureLoader::TextureLoader(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<DescriptorTableManager> descriptorTable)
    : m_Device(device)
    , m_DescriptorTable(std::move(descriptorTable))
    , m_fs(std::move(fs))
{
}

TextureLoader::~TextureLoader()
{
    Reset();
}

void TextureLoader::Reset()
{
    // The AssetSystem owns the single texture cache; the loader no longer keeps
    // its own. Unregister every cached texture from the registry, then clear.
    auto& registry = AssetSystem::Get().GetRegistry();
    auto& cache = AssetSystem::Get().GetTextureCache();
    cache.ForEach([&](const AssetId& id, std::shared_ptr<TextureData>, CacheState) {
        registry.Unregister(id);
    });

    cache.Clear();
    m_TexturesRequested = 0;
    m_TexturesLoaded = 0;
}

void TextureLoader::SetGenerateMipmaps(bool generateMipmaps)
{
    m_GenerateMipmaps = generateMipmaps;
}

void TextureLoader::RegisterTextureAsset(const std::shared_ptr<TextureData>& texture)
{
    if (texture->path.empty())
        return;

    auto& registry = AssetSystem::Get().GetRegistry();
    AssetId id = registry.Register(texture->path, AssetType::Texture);
    texture->assetIdLow = id.low;
    texture->assetIdHigh = id.high;
    registry.SetState(id, AssetState::Loaded);
}

bool TextureLoader::FindTextureInCache(const std::filesystem::path& path, std::shared_ptr<TextureData>& texture)
{
    auto& registry = AssetSystem::Get().GetRegistry();
    auto& cache = AssetSystem::Get().GetTextureCache();
    AssetId id = registry.FindByPath(path);

    // Check if already cached by AssetId (any state — a deferred texture that is
    // not yet GPU-finalized must still be found here).
    if (id.IsValid())
    {
        texture = cache.GetAny(id);
        if (texture)
            return true;
    }

    // Allocate a new texture slot.  LoadTextureFromFileAsync for a given scene
    // is only called from one thread, so no chance of double-load.
    texture = CreateTextureData();
    texture->path = path.generic_string();

    // Register with the asset system to get a stable AssetId
    if (!id.IsValid())
        id = registry.Register(path, AssetType::Texture);

    texture->assetIdLow = id.low;
    texture->assetIdHigh = id.high;
    cache.Insert(id, texture);

    ++m_TexturesRequested;
    return false;
}

std::shared_ptr<IBlob> TextureLoader::ReadTextureFile(const std::filesystem::path& path) const
{
    auto fileData = m_fs->readFile(path);

    if (!fileData)
        caustica::message(m_ErrorLogSeverity, "Couldn't read texture file '%s'", path.generic_string().c_str());

    return fileData;
}

std::shared_ptr<TextureData> TextureLoader::CreateTextureData()
{
    return std::make_shared<TextureData>();
}

bool TextureLoader::FillTextureData(
    const std::shared_ptr<caustica::IBlob>& fileData,
    const std::shared_ptr<TextureData>& texture,
    const std::string& extension,
    const std::string& mimeType) const
{
    if (extension == ".dds" || extension == ".DDS" || mimeType == "image/vnd-ms.dds")
    {
        texture->data = fileData;
        if (!LoadDDSTextureFromMemory(*texture))
        {
            texture->data = nullptr;
            caustica::message(m_ErrorLogSeverity, "Couldn't load DDS texture '%s'", texture->path.c_str());
            return false;
        }
    }
#ifdef CAUSTICA_WITH_TINYEXR
    else if (extension == ".exr" || extension == ".EXR" || mimeType == "image/aces")
    {
        float* data = nullptr;
        int width = 0, height = 0;
        char const* err = nullptr;

        // This reads only 1 or 4 channel images and duplicates channels
        // Should rewrite w/ lower level EXR functions
        if (LoadEXRFromMemory(&data, &width, &height, (uint8_t*)fileData->data(), fileData->size(), &err) == TINYEXR_SUCCESS)
        {
            uint32_t channels = 4;
            uint32_t bytesPerPixel = channels * 4;

            texture->data = std::make_shared<Blob>(data, bytesPerPixel * width * height);
            texture->width = static_cast<uint32_t>(width);
            texture->height = static_cast<uint32_t>(height);
            texture->format = nvrhi::Format::RGBA32_FLOAT;

            texture->originalBitsPerPixel = channels * 32;
            texture->isRenderTarget = true;
            texture->mipLevels = 1;
            texture->dimension = nvrhi::TextureDimension::Texture2D;

            texture->dataLayout.resize(1);
            texture->dataLayout[0].resize(1);
            texture->dataLayout[0][0].dataOffset = 0;
            texture->dataLayout[0][0].rowPitch = static_cast<size_t>(width * bytesPerPixel);
            texture->dataLayout[0][0].dataSize = static_cast<size_t>(width * height * bytesPerPixel);

            return true;
        }
        else
        {
            caustica::warning("Couldn't load EXR texture '%s'", texture->path.c_str());
            return false;
        }
    }
#endif // CAUSTICA_WITH_TINYEXR
    else
    {
        int width = 0, height = 0, originalChannels = 0, channels = 0;

        if (!stbi_info_from_memory(
            static_cast<const stbi_uc*>(fileData->data()), 
            static_cast<int>(fileData->size()), 
            &width, &height, &originalChannels))
        {
            caustica::message(m_ErrorLogSeverity, "Couldn't process image header for texture '%s'", texture->path.c_str());
            return false;
        }

        bool is_hdr = stbi_is_hdr_from_memory(
            static_cast<const stbi_uc*>(fileData->data()),
            static_cast<int>(fileData->size()));

        if (originalChannels == 3)
        {
            channels = 4;
        }
        else {
            channels = originalChannels;
        }

        unsigned char* bitmap;
        int bytesPerPixel = channels * (is_hdr ? 4 : 1);
        
        if (is_hdr)
        {
            float* floatmap = stbi_loadf_from_memory(
                static_cast<const stbi_uc*>(fileData->data()),
                static_cast<int>(fileData->size()),
                &width, &height, &originalChannels, channels);

            bitmap = reinterpret_cast<unsigned char*>(floatmap);
        }
        else
        {
            bitmap = stbi_load_from_memory(
                static_cast<const stbi_uc*>(fileData->data()),
                static_cast<int>(fileData->size()),
                &width, &height, &originalChannels, channels);
        }

        if (!bitmap)
        {
            caustica::message(m_ErrorLogSeverity, "Couldn't load generic texture '%s'", texture->path.c_str());
            return false;
        }

        texture->originalBitsPerPixel = static_cast<uint32_t>(originalChannels) * (is_hdr ? 32 : 8);
        texture->width = static_cast<uint32_t>(width);
        texture->height = static_cast<uint32_t>(height);
        texture->isRenderTarget = true;
        texture->mipLevels = 1;
        texture->dimension = nvrhi::TextureDimension::Texture2D;

        texture->dataLayout.resize(1);
        texture->dataLayout[0].resize(1);
        texture->dataLayout[0][0].dataOffset = 0;
        texture->dataLayout[0][0].rowPitch = static_cast<size_t>(width * bytesPerPixel);
        texture->dataLayout[0][0].dataSize = static_cast<size_t>(width * height * bytesPerPixel);

        texture->data = std::make_shared<StbImageBlob>(bitmap);
        bitmap = nullptr; // ownership transferred to the blob

        switch (channels)
        {
        case 1:
            texture->format = is_hdr ? nvrhi::Format::R32_FLOAT : nvrhi::Format::R8_UNORM;
            break;
        case 2:
            texture->format = is_hdr ? nvrhi::Format::RG32_FLOAT : nvrhi::Format::RG8_UNORM;
            break;
        case 4:
            texture->format = is_hdr ? nvrhi::Format::RGBA32_FLOAT :
                (texture->forceSRGB ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM);
            break;
        default:
            texture->data.reset(); // release the bitmap data

            caustica::message(m_ErrorLogSeverity, "Unsupported number of components (%d) for texture '%s'", channels, texture->path.c_str());
            return false;
        }
    }

    return true;
}

uint GetMipLevelsNum(uint width, uint height)
{
    uint size = std::min(width, height);
    uint levelsNum = (uint)(logf((float)size) / logf(2.0f)) + 1;

    return levelsNum;
}

void TextureLoader::FinalizeTexture(
    std::shared_ptr<TextureData> texture,
    CommonRenderPasses* passes,
    nvrhi::ICommandList* commandList)
{
    assert(texture->data);
    assert(commandList);

    uint originalWidth = texture->width;
    uint originalHeight = texture->height;

    bool isBlockCompressed =
        (texture->format == nvrhi::Format::BC1_UNORM) ||
        (texture->format == nvrhi::Format::BC1_UNORM_SRGB) ||
        (texture->format == nvrhi::Format::BC2_UNORM) ||
        (texture->format == nvrhi::Format::BC2_UNORM_SRGB) ||
        (texture->format == nvrhi::Format::BC3_UNORM) ||
        (texture->format == nvrhi::Format::BC3_UNORM_SRGB) ||
        (texture->format == nvrhi::Format::BC4_SNORM) ||
        (texture->format == nvrhi::Format::BC4_UNORM) ||
        (texture->format == nvrhi::Format::BC5_SNORM) ||
        (texture->format == nvrhi::Format::BC5_UNORM) ||
        (texture->format == nvrhi::Format::BC6H_SFLOAT) ||
        (texture->format == nvrhi::Format::BC6H_UFLOAT) ||
        (texture->format == nvrhi::Format::BC7_UNORM) ||
        (texture->format == nvrhi::Format::BC7_UNORM_SRGB);

    if (isBlockCompressed)
    {
        originalWidth = (originalWidth + 3) & ~3;
        originalHeight = (originalHeight + 3) & ~3;
    }

    uint scaledWidth = originalWidth;
    uint scaledHeight = originalHeight;

    if (m_MaxTextureSize > 0 && int(std::max(originalWidth, originalHeight)) > m_MaxTextureSize &&
        texture->isRenderTarget && texture->dimension == nvrhi::TextureDimension::Texture2D)
    {
        if (originalWidth >= originalHeight)
        {
            scaledHeight = originalHeight * m_MaxTextureSize / originalWidth;
            scaledWidth = m_MaxTextureSize;
        }
        else
        {
            scaledWidth = originalWidth * m_MaxTextureSize / originalHeight;
            scaledHeight = m_MaxTextureSize;
        }
    }

    const char* dataPointer = static_cast<const char*>(texture->data->data());

    nvrhi::TextureDesc textureDesc;
    textureDesc.format = texture->format;
    textureDesc.width = scaledWidth;
    textureDesc.height = scaledHeight;
    textureDesc.depth = texture->depth;
    textureDesc.arraySize = texture->arraySize;
    textureDesc.dimension = texture->dimension;
    textureDesc.mipLevels = m_GenerateMipmaps && texture->isRenderTarget && passes
        ? GetMipLevelsNum(textureDesc.width, textureDesc.height)
        : texture->mipLevels;
    textureDesc.debugName = texture->path;
    textureDesc.isRenderTarget = texture->isRenderTarget;
    texture->texture = m_Device->createTexture(textureDesc);

    commandList->beginTrackingTextureState(texture->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    if (m_DescriptorTable)
        texture->bindlessDescriptor = m_DescriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::Texture_SRV(0, texture->texture));
    
    if (scaledWidth != originalWidth || scaledHeight != originalHeight)
    {
        nvrhi::TextureDesc tempTextureDesc;
        tempTextureDesc.format = texture->format;
        tempTextureDesc.width = originalWidth;
        tempTextureDesc.height = originalHeight;
        tempTextureDesc.depth = textureDesc.depth;
        tempTextureDesc.arraySize = textureDesc.arraySize;
        tempTextureDesc.mipLevels = 1;
        tempTextureDesc.dimension = textureDesc.dimension;

        nvrhi::TextureHandle tempTexture = m_Device->createTexture(tempTextureDesc);
        assert(tempTexture);
        commandList->beginTrackingTextureState(tempTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

        for (uint32_t arraySlice = 0; arraySlice < texture->arraySize; arraySlice++)
        {
            const TextureSubresourceData& layout = texture->dataLayout[arraySlice][0];

            commandList->writeTexture(tempTexture, arraySlice, 0, dataPointer + layout.dataOffset,
                layout.rowPitch, layout.depthPitch);
        }

        nvrhi::FramebufferHandle framebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(texture->texture));
        
        passes->BlitTexture(commandList, framebuffer, tempTexture);
    }
    else
    {
        for (uint32_t arraySlice = 0; arraySlice < texture->arraySize; arraySlice++)
        {
            for (uint32_t mipLevel = 0; mipLevel < texture->mipLevels; mipLevel++)
            {
                const TextureSubresourceData& layout = texture->dataLayout[arraySlice][mipLevel];

                commandList->writeTexture(texture->texture, arraySlice, mipLevel, dataPointer + layout.dataOffset,
                    layout.rowPitch, layout.depthPitch);
            }
        }
    }

    texture->data.reset();

    for (uint mipLevel = texture->mipLevels; mipLevel < textureDesc.mipLevels; mipLevel++)
    {
        nvrhi::FramebufferHandle framebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(nvrhi::FramebufferAttachment()
                .setTexture(texture->texture)
                .setArraySlice(0)
                .setMipLevel(mipLevel)));
        
        BlitParameters blitParams;
        blitParams.sourceTexture = texture->texture;
        blitParams.sourceMip = mipLevel - 1;
        blitParams.targetFramebuffer = framebuffer;
        passes->BlitTexture(commandList, blitParams);
    }

    commandList->setPermanentTextureState(texture->texture, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    ++m_TexturesFinalized;
}

void TextureLoader::TextureLoaded(std::shared_ptr<TextureData> texture)
{
    std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

    if (texture->mimeType.empty())
        caustica::message(m_InfoLogSeverity, "Loaded %d x %d, %d bpp: %s", texture->width, texture->height,
        texture->originalBitsPerPixel, texture->path.c_str());
    else
        caustica::message(m_InfoLogSeverity, "Loaded %d x %d, %d bpp: %s (%s)", texture->width, texture->height,
        texture->originalBitsPerPixel, texture->path.c_str(), texture->mimeType.c_str());

    // Register with asset system (sets AssetId, updates state to Loaded)
    RegisterTextureAsset(texture);
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    CommonRenderPasses* passes,
    nvrhi::ICommandList* commandList)
{
    std::shared_ptr<TextureData> texture;

    if (FindTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    auto fileData = ReadTextureFile(path);
    if (fileData)
    {
        if (FillTextureData(fileData, texture, path.extension().generic_string(), ""))
        {
            TextureLoaded(texture);

            FinalizeTexture(texture, passes, commandList);
        }
    }

    ++m_TexturesLoaded;

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    std::shared_ptr<TextureData> texture;

    if (FindTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    auto fileData = ReadTextureFile(path);
    if (fileData)
    {
        if (FillTextureData(fileData, texture, path.extension().generic_string(), ""))
        {
            TextureLoaded(texture);

            std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

            m_TexturesToFinalize.push(texture);
        }
    }

    ++m_TexturesLoaded;

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB,
    ThreadPool& threadPool)
{
    std::shared_ptr<TextureData> texture;

    if (FindTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    threadPool.AddTask([this, texture, path]()
    {
        auto fileData = ReadTextureFile(path);
        if (fileData)
        {
            if (FillTextureData(fileData, texture, path.extension().generic_string(), ""))
            {
                TextureLoaded(texture);

                std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

                m_TexturesToFinalize.push(texture);
            }
        }

        ++m_TexturesLoaded;
    });

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromMemoryAsync(
    const std::shared_ptr<caustica::IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    ThreadPool& threadPool)
{
    std::shared_ptr<TextureData> texture = CreateTextureData();
    
    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    threadPool.AddTask([this, texture, data, mimeType]()
        {
            if (FillTextureData(data, texture, "", mimeType))
            {
                TextureLoaded(texture);

                std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

                m_TexturesToFinalize.push(texture);
            }

            ++m_TexturesLoaded;
        });

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromMemory(
    const std::shared_ptr<caustica::IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    CommonRenderPasses* passes,
    nvrhi::ICommandList* commandList)
{
    std::shared_ptr<TextureData> texture = CreateTextureData();
    
    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    if (FillTextureData(data, texture, "", mimeType))
    {
        TextureLoaded(texture);

        FinalizeTexture(texture, passes, commandList);
    }

    ++m_TexturesLoaded;

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::LoadTextureFromMemoryDeferred(
    const std::shared_ptr<caustica::IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    std::shared_ptr<TextureData> texture = CreateTextureData();
    
    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    if (FillTextureData(data, texture, "", mimeType))
    {
        TextureLoaded(texture);

        std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

        m_TexturesToFinalize.push(texture);
    }
    
    ++m_TexturesLoaded;

    return texture;
}


std::shared_ptr<TextureData> TextureLoader::GetLoadedTexture(std::filesystem::path const& path)
{
    AssetId id = AssetSystem::Get().GetRegistry().FindByPath(path);
    if (!id.IsValid())
        return nullptr;
    return AssetSystem::Get().GetTextureCache().GetAny(id);
}

bool TextureLoader::ProcessRenderingThreadCommands(CommonRenderPasses& passes, float timeLimitMilliseconds)
{
    using namespace std::chrono;

    time_point<high_resolution_clock> startTime = high_resolution_clock::now();

    uint commandsExecuted = 0;
    while (true)
    {
        std::shared_ptr<TextureData> pTexture;

        if (timeLimitMilliseconds > 0 && commandsExecuted > 0)
        {
            time_point<high_resolution_clock> now = high_resolution_clock::now();

            if (float(duration_cast<microseconds>(now - startTime).count()) > timeLimitMilliseconds * 1e3f)
                break;
        }

        {
            std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

            if (m_TexturesToFinalize.empty())
                break;

            pTexture = m_TexturesToFinalize.front();
            m_TexturesToFinalize.pop();
        }

        if (pTexture->data)
        {
            commandsExecuted += 1;

            if (!m_CommandList)
            {
                m_CommandList = m_Device->createCommandList();
            }

            m_CommandList->open();

            FinalizeTexture(pTexture, &passes, m_CommandList);

            m_CommandList->close();
            m_Device->executeCommandList(m_CommandList);
            m_Device->runGarbageCollection();
        }
    }

    return (commandsExecuted > 0);
}

void TextureLoader::LoadingFinished()
{
    m_CommandList = nullptr;
}

void TextureLoader::SetMaxTextureSize(uint32_t size)
{
	m_MaxTextureSize = size;
}

#ifdef _MSC_VER 
#define strcasecmp _stricmp
#endif

namespace caustica
{
    bool SaveTextureToFile(
        nvrhi::IDevice* device,
        CommonRenderPasses* pPasses,
        nvrhi::ITexture* texture,
        nvrhi::ResourceStates textureState,
        const char* fileName,
        bool saveAlphaChannel)
    {
        if (!fileName)
            return false;

        // Find the file's extension
        char const* ext = strrchr(fileName, '.');

        if (!ext)
            return false; // No extension fond in the file name

        // Determine the image format from the extension
        enum { BMP, PNG, JPG, TGA } destFormat;
        if (strcasecmp(ext, ".bmp") == 0)
            destFormat = BMP;
        else if (strcasecmp(ext, ".png") == 0)
            destFormat = PNG;
        else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
            destFormat = JPG;
        else if (strcasecmp(ext, ".tga") == 0)
            destFormat = TGA;
        else
            return false; // Unknown file type
        
        if (destFormat == JPG)
            saveAlphaChannel = false;

        nvrhi::TextureDesc desc = texture->getDesc();
        nvrhi::TextureHandle tempTexture;
        nvrhi::FramebufferHandle tempFramebuffer;

        nvrhi::CommandListHandle commandList = device->createCommandList();
        commandList->open();

        if (textureState != nvrhi::ResourceStates::Unknown)
        {
            commandList->beginTrackingTextureState(texture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
        }

        // If the source texture format is not RGBA8, create a temporary texture and blit into it to convert
        switch (desc.format)
        {
        case nvrhi::Format::RGBA8_UNORM:
        case nvrhi::Format::SRGBA8_UNORM:
            tempTexture = texture;
            break;
        default:
            desc.format = nvrhi::Format::SRGBA8_UNORM;
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.keepInitialState = true;

            tempTexture = device->createTexture(desc);
            tempFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(tempTexture));
            
            pPasses->BlitTexture(commandList, tempFramebuffer, texture);
        }

        // Create a plain staging texture for CPU readback. Do not inherit
        // render-target/UAV/typeless flags from intermediate render targets:
        // Vulkan is stricter about staging image usage than D3D12.
        nvrhi::TextureDesc stagingDesc = desc;
        stagingDesc.isRenderTarget = false;
        stagingDesc.isUAV = false;
        stagingDesc.isTypeless = false;
        stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
        stagingDesc.keepInitialState = true;
        stagingDesc.debugName = "SaveTextureToFile Staging";

        nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (!stagingTexture)
        {
            commandList->close();
            return false;
        }

        commandList->copyTexture(stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice());

        if (textureState != nvrhi::ResourceStates::Unknown)
        {
            commandList->setTextureState(texture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
            commandList->commitBarriers();
        }

        commandList->close();
        device->executeCommandList(commandList);

        // Ensure the copy finishes before CPU readback. Callers may invoke this
        // while the main frame command list is still in flight on the GPU.
        if (!device->waitForIdle())
            return false;

        // Map the staging texture
        size_t rowPitch = 0;
        uint8_t const* pData = static_cast<uint8_t const*>(device->mapStagingTexture(
            stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));

        if (!pData)
            return false;

        uint8_t* newData = nullptr;
        int channels = saveAlphaChannel ? 4 : 3;

        // If the mapped data is not laid out in a densely packed format with the right number of channels,
        // create a temporary buffer and move the data into the right layout for stb_image.
        if (rowPitch != desc.width * channels)
        {
            newData = new uint8_t[desc.width * desc.height * channels];

            for (uint32_t row = 0; row < desc.height; ++row)
            {
                uint8_t* dstRow = newData + row * desc.width * channels;
                uint8_t const* srcRow = pData + row * rowPitch;

                if (channels == 4)
                {
                    // Simple row copy
                    memcpy(dstRow, srcRow, desc.width * channels);
                }
                else
                {
                    // Convert 4 channels to 3
                    for (uint32_t col = 0; col < desc.width; ++col)
                    {
                        dstRow[0] = srcRow[0];
                        dstRow[1] = srcRow[1];
                        dstRow[2] = srcRow[2];
                        dstRow += 3;
                        srcRow += 4;
                    }
                }
            }

            pData = newData;
        }

        // Write the output image
        bool writeSuccess = false;
        switch(destFormat)
        {
            case BMP: 
                writeSuccess = stbi_write_bmp(fileName, int(desc.width), int(desc.height), channels, pData) != 0;
                break;
            case PNG: 
                writeSuccess = stbi_write_png(fileName, int(desc.width), int(desc.height), channels, pData, desc.width * channels) != 0;
                break;
            case JPG: 
                writeSuccess = stbi_write_jpg(fileName, int(desc.width), int(desc.height), channels, pData, /* quality = */ 99) != 0;
                break;
            case TGA: 
                writeSuccess = stbi_write_tga(fileName, int(desc.width), int(desc.height), channels, pData) != 0;
                break;
        }
        
        if (newData)
        {
            delete[] newData;
            newData = nullptr;
        }

        device->unmapStagingTexture(stagingTexture);

        return writeSuccess;
    }

    bool TextureLoader::IsTextureLoaded(const std::shared_ptr<LoadedTexture>& _texture)
    {
        TextureData* texture = static_cast<TextureData*>(_texture.get());

        return texture && texture->data;
    }

    bool TextureLoader::IsTextureFinalized(const std::shared_ptr<LoadedTexture>& texture)
    {
        return texture->texture != nullptr;
    }

    bool TextureLoader::UnloadTexture(const std::shared_ptr<LoadedTexture>& texture)
    {
        AssetId id{texture->assetIdLow, texture->assetIdHigh};
        if (!id.IsValid())
            return false;

        auto& cache = AssetSystem::Get().GetTextureCache();
        if (!cache.GetAny(id))
            return false;

        // Unregister from asset system and remove from the cache
        AssetSystem::Get().GetRegistry().Unregister(id);
        cache.Remove(id);
        return true;
    }

}
