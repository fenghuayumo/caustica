#include <assets/loader/TextureLoader.h>

#include <assets/loader/DDSFile.h>
#include <core/ThreadPool.h>
#include <core/vfs/VFS.h>
#include <core/log.h>

#include <stb_image.h>

#ifdef CAUSTICA_WITH_TINYEXR

    #if defined (_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable:4018)
    #endif

    #define TINYEXR_IMPLEMENTATION
    #include <tinyexr.h>

    #if defined (_MSC_VER)
        #pragma warning(pop)
    #endif

#endif // CAUSTICA_WITH_TINYEXR

#include <algorithm>

using namespace caustica;

class StbImageBlob : public IBlob
{
private:
    unsigned char* m_data = nullptr;

public:
    explicit StbImageBlob(unsigned char* data) : m_data(data) {}

    ~StbImageBlob() override
    {
        if (m_data)
        {
            stbi_image_free(m_data);
            m_data = nullptr;
        }
    }

    const void* data() const override { return m_data; }
    size_t size() const override { return 0; }
};

TextureLoader::TextureLoader(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<IDescriptorTableManager> descriptorTable,
    AssetRegistry& registry,
    AssetCache<TextureData>& cache)
    : m_Device(device)
    , m_DescriptorTable(std::move(descriptorTable))
    , m_fs(std::move(fs))
    , m_Registry(registry)
    , m_Cache(cache)
{
}

TextureLoader::~TextureLoader()
{
    reset();
}

void TextureLoader::reset()
{
    m_Cache.forEach([&](const AssetId& id, std::shared_ptr<TextureData>, CacheState) {
        m_Registry.unregisterAsset(id);
    });

    m_Cache.clear();
    m_TexturesRequested = 0;
    m_TexturesLoaded = 0;
}

void TextureLoader::setGenerateMipmaps(bool generateMipmaps)
{
    m_GenerateMipmaps = generateMipmaps;
}

void TextureLoader::registerTextureAsset(const std::shared_ptr<TextureData>& texture)
{
    if (texture->path.empty())
        return;

    AssetId id = m_Registry.registerAsset(texture->path, AssetType::Texture);
    texture->assetIdLow = id.low;
    texture->assetIdHigh = id.high;
    m_Registry.setState(id, AssetState::Loaded);
}

bool TextureLoader::findTextureInCache(const std::filesystem::path& path, std::shared_ptr<TextureData>& texture)
{
    AssetId id = m_Registry.findByPath(path);

    if (id.isValid())
    {
        texture = m_Cache.getAny(id);
        if (texture)
            return true;
    }

    texture = createTextureData();
    texture->path = path.generic_string();

    if (!id.isValid())
        id = m_Registry.registerAsset(path, AssetType::Texture);

    texture->assetIdLow = id.low;
    texture->assetIdHigh = id.high;
    m_Cache.insert(id, texture);

    ++m_TexturesRequested;
    return false;
}

std::shared_ptr<IBlob> TextureLoader::readTextureFile(const std::filesystem::path& path) const
{
    auto fileData = m_fs->readFile(path);

    if (!fileData)
        message(m_ErrorLogSeverity, "Couldn't read texture file '%s'", path.generic_string().c_str());

    return fileData;
}

std::shared_ptr<TextureData> TextureLoader::createTextureData()
{
    return std::make_shared<TextureData>();
}

bool TextureLoader::fillTextureData(
    const std::shared_ptr<IBlob>& fileData,
    const std::shared_ptr<TextureData>& texture,
    const std::string& extension,
    const std::string& mimeType) const
{
    if (extension == ".dds" || extension == ".DDS" || mimeType == "image/vnd-ms.dds")
    {
        texture->data = fileData;
        if (!loadDDSTextureFromMemory(*texture))
        {
            texture->data = nullptr;
            message(m_ErrorLogSeverity, "Couldn't load DDS texture '%s'", texture->path.c_str());
            return false;
        }
    }
#ifdef CAUSTICA_WITH_TINYEXR
    else if (extension == ".exr" || extension == ".EXR" || mimeType == "image/aces")
    {
        float* data = nullptr;
        int width = 0, height = 0;
        char const* err = nullptr;

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

        warning("Couldn't load EXR texture '%s'", texture->path.c_str());
        return false;
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
            message(m_ErrorLogSeverity, "Couldn't process image header for texture '%s'", texture->path.c_str());
            return false;
        }

        bool is_hdr = stbi_is_hdr_from_memory(
            static_cast<const stbi_uc*>(fileData->data()),
            static_cast<int>(fileData->size()));

        if (originalChannels == 3)
            channels = 4;
        else
            channels = originalChannels;

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
            message(m_ErrorLogSeverity, "Couldn't load generic texture '%s'", texture->path.c_str());
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
            texture->data.reset();
            message(m_ErrorLogSeverity, "Unsupported number of components (%d) for texture '%s'", channels, texture->path.c_str());
            return false;
        }
    }

    return true;
}

void TextureLoader::textureLoaded(std::shared_ptr<TextureData> texture)
{
    std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);

    if (texture->mimeType.empty())
        message(m_InfoLogSeverity, "Loaded %d x %d, %d bpp: %s", texture->width, texture->height,
            texture->originalBitsPerPixel, texture->path.c_str());
    else
        message(m_InfoLogSeverity, "Loaded %d x %d, %d bpp: %s (%s)", texture->width, texture->height,
            texture->originalBitsPerPixel, texture->path.c_str(), texture->mimeType.c_str());

    registerTextureAsset(texture);
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    std::shared_ptr<TextureData> texture;

    if (findTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    auto fileData = readTextureFile(path);
    if (fileData)
    {
        if (fillTextureData(fileData, texture, path.extension().generic_string(), ""))
        {
            textureLoaded(texture);
            finalizeTexture(texture, renderDevice, commandList);
        }
    }

    ++m_TexturesLoaded;
    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    std::shared_ptr<TextureData> texture;

    if (findTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    auto fileData = readTextureFile(path);
    if (fileData)
    {
        if (fillTextureData(fileData, texture, path.extension().generic_string(), ""))
        {
            textureLoaded(texture);

            std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);
            m_TexturesToFinalize.push(texture);
        }
    }

    ++m_TexturesLoaded;
    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB,
    ThreadPool& threadPool)
{
    std::shared_ptr<TextureData> texture;

    if (findTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    threadPool.AddTask([this, texture, path]()
    {
        auto fileData = readTextureFile(path);
        if (fileData)
        {
            if (fillTextureData(fileData, texture, path.extension().generic_string(), ""))
            {
                textureLoaded(texture);

                std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);
                m_TexturesToFinalize.push(texture);
            }
        }

        ++m_TexturesLoaded;
    });

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromMemoryAsync(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    ThreadPool& threadPool)
{
    std::shared_ptr<TextureData> texture = createTextureData();

    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    threadPool.AddTask([this, texture, data, mimeType]()
    {
        if (fillTextureData(data, texture, "", mimeType))
        {
            textureLoaded(texture);

            std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);
            m_TexturesToFinalize.push(texture);
        }

        ++m_TexturesLoaded;
    });

    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromMemory(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    std::shared_ptr<TextureData> texture = createTextureData();

    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    if (fillTextureData(data, texture, "", mimeType))
    {
        textureLoaded(texture);
        finalizeTexture(texture, renderDevice, commandList);
    }

    ++m_TexturesLoaded;
    return texture;
}

std::shared_ptr<LoadedTexture> TextureLoader::loadTextureFromMemoryDeferred(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    std::shared_ptr<TextureData> texture = createTextureData();

    texture->forceSRGB = sRGB;
    texture->path = name;
    texture->mimeType = mimeType;

    if (fillTextureData(data, texture, "", mimeType))
    {
        textureLoaded(texture);

        std::lock_guard<std::mutex> guard(m_TexturesToFinalizeMutex);
        m_TexturesToFinalize.push(texture);
    }

    ++m_TexturesLoaded;
    return texture;
}

std::shared_ptr<TextureData> TextureLoader::getLoadedTexture(std::filesystem::path const& path)
{
    AssetId id = m_Registry.findByPath(path);
    if (!id.isValid())
        return nullptr;
    return m_Cache.getAny(id);
}

void TextureLoader::setMaxTextureSize(uint32_t size)
{
    m_MaxTextureSize = size;
}

bool TextureLoader::isTextureLoaded(const std::shared_ptr<LoadedTexture>& loadedTexture)
{
    auto* texture = static_cast<TextureData*>(loadedTexture.get());
    return texture && texture->data;
}

bool TextureLoader::isTextureFinalized(const std::shared_ptr<LoadedTexture>& texture)
{
    return texture->texture != nullptr;
}

bool TextureLoader::unloadTexture(const std::shared_ptr<LoadedTexture>& texture)
{
    AssetId id{texture->assetIdLow, texture->assetIdHigh};
    if (!id.isValid())
        return false;

    if (!m_Cache.getAny(id))
        return false;

    m_Registry.unregisterAsset(id);
    m_Cache.remove(id);
    return true;
}
