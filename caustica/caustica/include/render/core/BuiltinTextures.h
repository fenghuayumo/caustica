#pragma once

#include <rhi/nvrhi.h>

namespace caustica::render
{

class BuiltinTextures
{
public:
    explicit BuiltinTextures(nvrhi::IDevice* device);

    nvrhi::TextureHandle blackTexture() const { return m_blackTexture; }
    nvrhi::TextureHandle grayTexture() const { return m_grayTexture; }
    nvrhi::TextureHandle whiteTexture() const { return m_whiteTexture; }
    nvrhi::TextureHandle blackTexture2DArray() const { return m_blackTexture2DArray; }
    nvrhi::TextureHandle whiteTexture2DArray() const { return m_whiteTexture2DArray; }
    nvrhi::TextureHandle blackCubeMapArray() const { return m_blackCubeMapArray; }
    nvrhi::TextureHandle blackTexture3D() const { return m_blackTexture3D; }
    nvrhi::TextureHandle blackDepthStencilTexture() const { return m_blackDepthStencilTexture; }
    nvrhi::TextureHandle blackDepthStencilTexture2DArray() const { return m_blackDepthStencilTexture2DArray; }

private:
    nvrhi::DeviceHandle m_device;
    nvrhi::TextureHandle m_blackTexture;
    nvrhi::TextureHandle m_grayTexture;
    nvrhi::TextureHandle m_whiteTexture;
    nvrhi::TextureHandle m_blackTexture2DArray;
    nvrhi::TextureHandle m_whiteTexture2DArray;
    nvrhi::TextureHandle m_blackCubeMapArray;
    nvrhi::TextureHandle m_blackTexture3D;
    nvrhi::TextureHandle m_blackDepthStencilTexture;
    nvrhi::TextureHandle m_blackDepthStencilTexture2DArray;
};

} // namespace caustica::render
