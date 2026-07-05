#include <render/Core/BuiltinTextures.h>

namespace caustica::render
{

BuiltinTextures::BuiltinTextures(nvrhi::IDevice* device)
    : m_device(device)
{
    const unsigned int blackImage = 0xff000000;
    const unsigned int grayImage = 0xff808080;
    const unsigned int whiteImage = 0xffffffff;

    nvrhi::TextureDesc textureDesc;
    textureDesc.format = nvrhi::Format::RGBA8_UNORM;
    textureDesc.width = 1;
    textureDesc.height = 1;
    textureDesc.mipLevels = 1;

    textureDesc.debugName = "BlackTexture";
    m_blackTexture = m_device->createTexture(textureDesc);

    textureDesc.debugName = "GrayTexture";
    m_grayTexture = m_device->createTexture(textureDesc);

    textureDesc.debugName = "WhiteTexture";
    m_whiteTexture = m_device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::TextureCubeArray;
    textureDesc.debugName = "BlackCubeMapArray";
    textureDesc.arraySize = 6;
    m_blackCubeMapArray = m_device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
    textureDesc.debugName = "BlackTexture2DArray";
    textureDesc.arraySize = 1;
    m_blackTexture2DArray = m_device->createTexture(textureDesc);
    textureDesc.debugName = "WhiteTexture2DArray";
    m_whiteTexture2DArray = m_device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture3D;
    textureDesc.debugName = "BlackTexture3D";
    m_blackTexture3D = m_device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    textureDesc.format = nvrhi::Format::D24S8;
    textureDesc.isRenderTarget = true;
    textureDesc.isTypeless = true;
    textureDesc.debugName = "BlackDepthStencilTexture";
    m_blackDepthStencilTexture = m_device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
    textureDesc.debugName = "BlackDepthStencilTexture2DArray";
    m_blackDepthStencilTexture2DArray = m_device->createTexture(textureDesc);

    nvrhi::CommandListHandle commandList = m_device->createCommandList();
    commandList->open();

    commandList->beginTrackingTextureState(m_blackTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_grayTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_whiteTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_blackCubeMapArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_blackTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_whiteTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_blackTexture3D, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_blackDepthStencilTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_blackDepthStencilTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    commandList->writeTexture(m_blackTexture, 0, 0, &blackImage, 0);
    commandList->writeTexture(m_grayTexture, 0, 0, &grayImage, 0);
    commandList->writeTexture(m_whiteTexture, 0, 0, &whiteImage, 0);
    commandList->writeTexture(m_blackTexture2DArray, 0, 0, &blackImage, 0);
    commandList->writeTexture(m_whiteTexture2DArray, 0, 0, &whiteImage, 0);
    commandList->writeTexture(m_blackTexture3D, 0, 0, &blackImage, 0);
    commandList->clearDepthStencilTexture(m_blackDepthStencilTexture, nvrhi::AllSubresources, true, 0.f, true, 0);
    commandList->clearDepthStencilTexture(m_blackDepthStencilTexture2DArray, nvrhi::AllSubresources, true, 0.f, true, 0);

    for (uint32_t arraySlice = 0; arraySlice < 6; arraySlice += 1)
        commandList->writeTexture(m_blackCubeMapArray, arraySlice, 0, &blackImage, 0);

    commandList->setPermanentTextureState(m_blackTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_grayTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_whiteTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_blackCubeMapArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_blackTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_whiteTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_blackTexture3D, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_blackDepthStencilTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_blackDepthStencilTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    commandList->close();
    m_device->executeCommandList(commandList);
}

} // namespace caustica::render
