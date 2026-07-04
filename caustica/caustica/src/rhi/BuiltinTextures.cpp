#include <rhi/BuiltinTextures.h>

namespace caustica::rhi
{

BuiltinTextures::BuiltinTextures(nvrhi::IDevice* device)
    : m_Device(device)
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
    m_BlackTexture = m_Device->createTexture(textureDesc);

    textureDesc.debugName = "GrayTexture";
    m_GrayTexture = m_Device->createTexture(textureDesc);

    textureDesc.debugName = "WhiteTexture";
    m_WhiteTexture = m_Device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::TextureCubeArray;
    textureDesc.debugName = "BlackCubeMapArray";
    textureDesc.arraySize = 6;
    m_BlackCubeMapArray = m_Device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
    textureDesc.debugName = "BlackTexture2DArray";
    textureDesc.arraySize = 1;
    m_BlackTexture2DArray = m_Device->createTexture(textureDesc);
    textureDesc.debugName = "WhiteTexture2DArray";
    m_WhiteTexture2DArray = m_Device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture3D;
    textureDesc.debugName = "BlackTexture3D";
    m_BlackTexture3D = m_Device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    textureDesc.format = nvrhi::Format::D24S8;
    textureDesc.isRenderTarget = true;
    textureDesc.isTypeless = true;
    textureDesc.debugName = "BlackDepthStencilTexture";
    m_BlackDepthStencilTexture = m_Device->createTexture(textureDesc);

    textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
    textureDesc.debugName = "BlackDepthStencilTexture2DArray";
    m_BlackDepthStencilTexture2DArray = m_Device->createTexture(textureDesc);

    nvrhi::CommandListHandle commandList = m_Device->createCommandList();
    commandList->open();

    commandList->beginTrackingTextureState(m_BlackTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_GrayTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_WhiteTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_BlackCubeMapArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_BlackTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_WhiteTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_BlackTexture3D, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_BlackDepthStencilTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    commandList->beginTrackingTextureState(m_BlackDepthStencilTexture2DArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    commandList->writeTexture(m_BlackTexture, 0, 0, &blackImage, 0);
    commandList->writeTexture(m_GrayTexture, 0, 0, &grayImage, 0);
    commandList->writeTexture(m_WhiteTexture, 0, 0, &whiteImage, 0);
    commandList->writeTexture(m_BlackTexture2DArray, 0, 0, &blackImage, 0);
    commandList->writeTexture(m_WhiteTexture2DArray, 0, 0, &whiteImage, 0);
    commandList->writeTexture(m_BlackTexture3D, 0, 0, &blackImage, 0);
    commandList->clearDepthStencilTexture(m_BlackDepthStencilTexture, nvrhi::AllSubresources, true, 0.f, true, 0);
    commandList->clearDepthStencilTexture(m_BlackDepthStencilTexture2DArray, nvrhi::AllSubresources, true, 0.f, true, 0);

    for (uint32_t arraySlice = 0; arraySlice < 6; arraySlice += 1)
        commandList->writeTexture(m_BlackCubeMapArray, arraySlice, 0, &blackImage, 0);

    commandList->setPermanentTextureState(m_BlackTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_GrayTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_WhiteTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_BlackCubeMapArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_BlackTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_WhiteTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_BlackTexture3D, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_BlackDepthStencilTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_BlackDepthStencilTexture2DArray, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    commandList->close();
    m_Device->executeCommandList(commandList);
}

} // namespace caustica::rhi
