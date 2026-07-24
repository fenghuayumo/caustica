#include <assets/loader/TextureLoader.h>

#include <render/core/RenderDevice.h>
#include <render/core/FullscreenBlitPass.h>
#include <core/vfs/VFS.h>

#include <stb_image_write.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

namespace caustica
{

namespace
{

uint32_t GetMipLevelsNum(uint32_t width, uint32_t height)
{
    uint32_t size = std::min(width, height);
    return static_cast<uint32_t>(logf(static_cast<float>(size)) / logf(2.0f)) + 1;
}

} // namespace

void TextureLoader::finalizeTexture(
    std::shared_ptr<ImageAsset> texture,
    render::RenderDevice* renderDevice,
    caustica::rhi::ICommandList* commandList)
{
    assert(texture->data);
    assert(commandList);

    uint32_t originalWidth = texture->width;
    uint32_t originalHeight = texture->height;

    bool isBlockCompressed =
        (texture->format == caustica::rhi::Format::BC1_UNORM) ||
        (texture->format == caustica::rhi::Format::BC1_UNORM_SRGB) ||
        (texture->format == caustica::rhi::Format::BC2_UNORM) ||
        (texture->format == caustica::rhi::Format::BC2_UNORM_SRGB) ||
        (texture->format == caustica::rhi::Format::BC3_UNORM) ||
        (texture->format == caustica::rhi::Format::BC3_UNORM_SRGB) ||
        (texture->format == caustica::rhi::Format::BC4_SNORM) ||
        (texture->format == caustica::rhi::Format::BC4_UNORM) ||
        (texture->format == caustica::rhi::Format::BC5_SNORM) ||
        (texture->format == caustica::rhi::Format::BC5_UNORM) ||
        (texture->format == caustica::rhi::Format::BC6H_SFLOAT) ||
        (texture->format == caustica::rhi::Format::BC6H_UFLOAT) ||
        (texture->format == caustica::rhi::Format::BC7_UNORM) ||
        (texture->format == caustica::rhi::Format::BC7_UNORM_SRGB);

    if (isBlockCompressed)
    {
        originalWidth = (originalWidth + 3) & ~3;
        originalHeight = (originalHeight + 3) & ~3;
    }

    uint32_t scaledWidth = originalWidth;
    uint32_t scaledHeight = originalHeight;

    if (m_MaxTextureSize > 0 && int(std::max(originalWidth, originalHeight)) > m_MaxTextureSize &&
        texture->isRenderTarget && texture->dimension == caustica::rhi::TextureDimension::Texture2D)
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

    caustica::rhi::TextureDesc textureDesc;
    textureDesc.format = texture->format;
    textureDesc.width = scaledWidth;
    textureDesc.height = scaledHeight;
    textureDesc.depth = texture->depth;
    textureDesc.arraySize = texture->arraySize;
    textureDesc.dimension = texture->dimension;
    textureDesc.mipLevels = m_GenerateMipmaps && texture->isRenderTarget && renderDevice
        ? GetMipLevelsNum(textureDesc.width, textureDesc.height)
        : texture->mipLevels;
    textureDesc.debugName = texture->path;
    textureDesc.isRenderTarget = texture->isRenderTarget;
    texture->gpu.texture = m_Device->createTexture(textureDesc);

    commandList->beginTrackingTextureState(texture->gpu.texture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);

    if (m_DescriptorTable)
        texture->gpu.bindlessDescriptor = m_DescriptorTable->createDescriptorHandle(
            caustica::rhi::BindingSetItem::Texture_SRV(0, texture->gpu.texture));

    if (scaledWidth != originalWidth || scaledHeight != originalHeight)
    {
        caustica::rhi::TextureDesc tempTextureDesc;
        tempTextureDesc.format = texture->format;
        tempTextureDesc.width = originalWidth;
        tempTextureDesc.height = originalHeight;
        tempTextureDesc.depth = textureDesc.depth;
        tempTextureDesc.arraySize = textureDesc.arraySize;
        tempTextureDesc.mipLevels = 1;
        tempTextureDesc.dimension = textureDesc.dimension;

        caustica::rhi::TextureHandle tempTexture = m_Device->createTexture(tempTextureDesc);
        assert(tempTexture);
        commandList->beginTrackingTextureState(tempTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);

        for (uint32_t arraySlice = 0; arraySlice < texture->arraySize; arraySlice++)
        {
            const TextureSubresourceData& layout = texture->dataLayout[arraySlice][0];

            commandList->writeTexture(tempTexture, arraySlice, 0, dataPointer + layout.dataOffset,
                layout.rowPitch, layout.depthPitch);
        }

        caustica::rhi::FramebufferHandle framebuffer = m_Device->createFramebuffer(
            caustica::rhi::FramebufferDesc().addColorAttachment(texture->gpu.texture));

        renderDevice->blit().blitTexture(commandList, framebuffer, tempTexture);
    }
    else
    {
        for (uint32_t arraySlice = 0; arraySlice < texture->arraySize; arraySlice++)
        {
            for (uint32_t mipLevel = 0; mipLevel < texture->mipLevels; mipLevel++)
            {
                const TextureSubresourceData& layout = texture->dataLayout[arraySlice][mipLevel];

                commandList->writeTexture(texture->gpu.texture, arraySlice, mipLevel, dataPointer + layout.dataOffset,
                    layout.rowPitch, layout.depthPitch);
            }
        }
    }

    texture->data.reset();

    for (uint32_t mipLevel = texture->mipLevels; mipLevel < textureDesc.mipLevels; mipLevel++)
    {
        caustica::rhi::FramebufferHandle framebuffer = m_Device->createFramebuffer(caustica::rhi::FramebufferDesc()
            .addColorAttachment(caustica::rhi::FramebufferAttachment()
                .setTexture(texture->gpu.texture)
                .setArraySlice(0)
                .setMipLevel(mipLevel)));

        render::BlitParameters blitParams;
        blitParams.sourceTexture = texture->gpu.texture;
        blitParams.sourceMip = mipLevel - 1;
        blitParams.targetFramebuffer = framebuffer;
        renderDevice->blit().blitTexture(commandList, blitParams);
    }

    commandList->setPermanentTextureState(texture->gpu.texture, caustica::rhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    ++m_TexturesFinalized;
}

bool TextureLoader::processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds)
{
    using namespace std::chrono;

    time_point<high_resolution_clock> startTime = high_resolution_clock::now();

    uint32_t commandsExecuted = 0;
    while (true)
    {
        std::shared_ptr<ImageAsset> pTexture;

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
                m_CommandList = m_Device->createCommandList();

            m_CommandList->open();
            finalizeTexture(pTexture, &renderDevice, m_CommandList);
            m_CommandList->close();
            m_Device->executeCommandList(m_CommandList);
            m_Device->runGarbageCollection();
        }
    }

    return commandsExecuted > 0;
}

void TextureLoader::loadingFinished()
{
    m_CommandList = nullptr;
}

bool saveTextureToFile(
    caustica::rhi::IDevice* device,
    render::RenderDevice& renderDevice,
    caustica::rhi::ITexture* texture,
    caustica::rhi::ResourceStates textureState,
    const char* fileName,
    bool saveAlphaChannel)
{
    if (!fileName)
        return false;

    char const* ext = strrchr(fileName, '.');
    if (!ext)
        return false;

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
        return false;

    if (destFormat == JPG)
        saveAlphaChannel = false;

    caustica::rhi::TextureDesc desc = texture->getDesc();
    caustica::rhi::TextureHandle tempTexture;
    caustica::rhi::FramebufferHandle tempFramebuffer;

    caustica::rhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->beginTrackingTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
    }

    switch (desc.format)
    {
    case caustica::rhi::Format::RGBA8_UNORM:
    case caustica::rhi::Format::SRGBA8_UNORM:
        tempTexture = texture;
        break;
    default:
        desc.format = caustica::rhi::Format::SRGBA8_UNORM;
        desc.isRenderTarget = true;
        desc.initialState = caustica::rhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;

        tempTexture = device->createTexture(desc);
        tempFramebuffer = device->createFramebuffer(caustica::rhi::FramebufferDesc().addColorAttachment(tempTexture));

        renderDevice.blit().blitTexture(commandList, tempFramebuffer, texture);
    }

    caustica::rhi::TextureDesc stagingDesc = desc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.isTypeless = false;
    stagingDesc.initialState = caustica::rhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "saveTextureToFile Staging";

    caustica::rhi::StagingTextureHandle stagingTexture = device->createStagingTexture(stagingDesc, caustica::rhi::CpuAccessMode::Read);
    if (!stagingTexture)
    {
        commandList->close();
        return false;
    }

    commandList->copyTexture(stagingTexture, caustica::rhi::TextureSlice(), tempTexture, caustica::rhi::TextureSlice());

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->setTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
        commandList->commitBarriers();
    }

    commandList->close();
    device->executeCommandList(commandList);

    if (!device->waitForIdle())
        return false;

    size_t rowPitch = 0;
    uint8_t const* pData = static_cast<uint8_t const*>(device->mapStagingTexture(
        stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));

    if (!pData)
        return false;

    uint8_t* newData = nullptr;
    int channels = saveAlphaChannel ? 4 : 3;

    if (rowPitch != desc.width * channels)
    {
        newData = new uint8_t[desc.width * desc.height * channels];

        for (uint32_t row = 0; row < desc.height; ++row)
        {
            uint8_t* dstRow = newData + row * desc.width * channels;
            uint8_t const* srcRow = pData + row * rowPitch;

            if (channels == 4)
            {
                memcpy(dstRow, srcRow, desc.width * channels);
            }
            else
            {
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

    bool writeSuccess = false;
    switch (destFormat)
    {
    case BMP:
        writeSuccess = stbi_write_bmp(fileName, int(desc.width), int(desc.height), channels, pData) != 0;
        break;
    case PNG:
        writeSuccess = stbi_write_png(fileName, int(desc.width), int(desc.height), channels, pData, desc.width * channels) != 0;
        break;
    case JPG:
        writeSuccess = stbi_write_jpg(fileName, int(desc.width), int(desc.height), channels, pData, 99) != 0;
        break;
    case TGA:
        writeSuccess = stbi_write_tga(fileName, int(desc.width), int(desc.height), channels, pData) != 0;
        break;
    }

    if (newData)
        delete[] newData;

    device->unmapStagingTexture(stagingTexture);
    return writeSuccess;
}

} // namespace caustica
