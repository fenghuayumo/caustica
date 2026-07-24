#pragma once

#include <rhi/rhi.h>

namespace caustica::render
{

class BuiltinTextures
{
public:
    explicit BuiltinTextures(caustica::rhi::IDevice* device);

    caustica::rhi::TextureHandle blackTexture() const { return m_blackTexture; }
    caustica::rhi::TextureHandle grayTexture() const { return m_grayTexture; }
    caustica::rhi::TextureHandle whiteTexture() const { return m_whiteTexture; }
    caustica::rhi::TextureHandle blackTexture2DArray() const { return m_blackTexture2DArray; }
    caustica::rhi::TextureHandle whiteTexture2DArray() const { return m_whiteTexture2DArray; }
    caustica::rhi::TextureHandle blackCubeMapArray() const { return m_blackCubeMapArray; }
    caustica::rhi::TextureHandle blackTexture3D() const { return m_blackTexture3D; }
    caustica::rhi::TextureHandle blackDepthStencilTexture() const { return m_blackDepthStencilTexture; }
    caustica::rhi::TextureHandle blackDepthStencilTexture2DArray() const { return m_blackDepthStencilTexture2DArray; }

private:
    caustica::rhi::DeviceHandle m_device;
    caustica::rhi::TextureHandle m_blackTexture;
    caustica::rhi::TextureHandle m_grayTexture;
    caustica::rhi::TextureHandle m_whiteTexture;
    caustica::rhi::TextureHandle m_blackTexture2DArray;
    caustica::rhi::TextureHandle m_whiteTexture2DArray;
    caustica::rhi::TextureHandle m_blackCubeMapArray;
    caustica::rhi::TextureHandle m_blackTexture3D;
    caustica::rhi::TextureHandle m_blackDepthStencilTexture;
    caustica::rhi::TextureHandle m_blackDepthStencilTexture2DArray;
};

} // namespace caustica::render
