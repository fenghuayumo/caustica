#include <render/Core/CommonRenderPasses.h>

#include <rhi/BuiltinTextures.h>
#include <rhi/FullscreenBlitPass.h>
#include <rhi/StandardSamplers.h>

namespace caustica
{

CommonRenderPasses::CommonRenderPasses(rhi::BuiltinTextures& builtins,
    rhi::StandardSamplers& samplers,
    rhi::FullscreenBlitPass& blit)
    : m_Blit(&blit)
{
    m_FullscreenVS = blit.fullscreenVS();
    m_FullscreenAtOneVS = blit.fullscreenAtOneVS();
    m_RectVS = blit.rectVS();
    m_BlitPS = blit.blitPS();
    m_BlitArrayPS = blit.blitArrayPS();
    m_SharpenPS = blit.sharpenPS();
    m_SharpenArrayPS = blit.sharpenArrayPS();
    m_BlitBindingLayout = blit.blitBindingLayout();

    m_BlackTexture = builtins.blackTexture();
    m_GrayTexture = builtins.grayTexture();
    m_WhiteTexture = builtins.whiteTexture();
    m_BlackTexture2DArray = builtins.blackTexture2DArray();
    m_WhiteTexture2DArray = builtins.whiteTexture2DArray();
    m_BlackCubeMapArray = builtins.blackCubeMapArray();
    m_BlackTexture3D = builtins.blackTexture3D();
    m_BlackDepthStencilTexture = builtins.blackDepthStencilTexture();
    m_BlackDepthStencilTexture2DArray = builtins.blackDepthStencilTexture2DArray();

    m_PointClampSampler = samplers.pointClamp();
    m_LinearClampSampler = samplers.linearClamp();
    m_LinearWrapSampler = samplers.linearWrap();
    m_AnisotropicWrapSampler = samplers.anisotropicWrap();
}

void CommonRenderPasses::BlitTexture(nvrhi::ICommandList* commandList, const BlitParameters& params, BindingCache* bindingCache)
{
    m_Blit->blitTexture(commandList, params, bindingCache);
}

void CommonRenderPasses::BlitTexture(nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* targetFramebuffer,
    nvrhi::ITexture* sourceTexture,
    BindingCache* bindingCache)
{
    m_Blit->blitTexture(commandList, targetFramebuffer, sourceTexture, bindingCache);
}

} // namespace caustica
