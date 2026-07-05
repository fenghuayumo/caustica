#pragma once

#include <rhi/nvrhi.h>

namespace caustica::render
{

class BuiltinTextures
{
public:
    explicit BuiltinTextures(nvrhi::IDevice* device);

    nvrhi::TextureHandle blackTexture() const { return m_BlackTexture; }
    nvrhi::TextureHandle grayTexture() const { return m_GrayTexture; }
    nvrhi::TextureHandle whiteTexture() const { return m_WhiteTexture; }
    nvrhi::TextureHandle blackTexture2DArray() const { return m_BlackTexture2DArray; }
    nvrhi::TextureHandle whiteTexture2DArray() const { return m_WhiteTexture2DArray; }
    nvrhi::TextureHandle blackCubeMapArray() const { return m_BlackCubeMapArray; }
    nvrhi::TextureHandle blackTexture3D() const { return m_BlackTexture3D; }
    nvrhi::TextureHandle blackDepthStencilTexture() const { return m_BlackDepthStencilTexture; }
    nvrhi::TextureHandle blackDepthStencilTexture2DArray() const { return m_BlackDepthStencilTexture2DArray; }

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::TextureHandle m_BlackTexture;
    nvrhi::TextureHandle m_GrayTexture;
    nvrhi::TextureHandle m_WhiteTexture;
    nvrhi::TextureHandle m_BlackTexture2DArray;
    nvrhi::TextureHandle m_WhiteTexture2DArray;
    nvrhi::TextureHandle m_BlackCubeMapArray;
    nvrhi::TextureHandle m_BlackTexture3D;
    nvrhi::TextureHandle m_BlackDepthStencilTexture;
    nvrhi::TextureHandle m_BlackDepthStencilTexture2DArray;
};

} // namespace caustica::render
