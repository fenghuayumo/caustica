#pragma once

#include <rhi/FullscreenBlitPass.h>
#include <rhi/nvrhi.h>

namespace caustica::rhi
{
class BuiltinTextures;
class FullscreenBlitPass;
class StandardSamplers;
}

namespace caustica
{

constexpr uint32_t c_MaxRenderPassConstantBufferVersions = 16;

using BlitSampler = rhi::BlitSampler;
using BlitParameters = rhi::BlitParameters;

// Legacy facade over rhi::BuiltinTextures / StandardSamplers / FullscreenBlitPass.
// Public members mirror the pre-R0 layout for existing render passes.
class CommonRenderPasses
{
public:
    CommonRenderPasses(rhi::BuiltinTextures& builtins, rhi::StandardSamplers& samplers, rhi::FullscreenBlitPass& blit);

    void BlitTexture(nvrhi::ICommandList* commandList, const BlitParameters& params, BindingCache* bindingCache = nullptr);
    void BlitTexture(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* targetFramebuffer, nvrhi::ITexture* sourceTexture, BindingCache* bindingCache = nullptr);

    nvrhi::ShaderHandle m_FullscreenVS;
    nvrhi::ShaderHandle m_FullscreenAtOneVS;
    nvrhi::ShaderHandle m_RectVS;
    nvrhi::ShaderHandle m_BlitPS;
    nvrhi::ShaderHandle m_BlitArrayPS;
    nvrhi::ShaderHandle m_SharpenPS;
    nvrhi::ShaderHandle m_SharpenArrayPS;

    nvrhi::TextureHandle m_BlackTexture;
    nvrhi::TextureHandle m_GrayTexture;
    nvrhi::TextureHandle m_WhiteTexture;
    nvrhi::TextureHandle m_BlackTexture2DArray;
    nvrhi::TextureHandle m_WhiteTexture2DArray;
    nvrhi::TextureHandle m_BlackCubeMapArray;
    nvrhi::TextureHandle m_BlackTexture3D;
    nvrhi::TextureHandle m_BlackDepthStencilTexture;
    nvrhi::TextureHandle m_BlackDepthStencilTexture2DArray;

    nvrhi::SamplerHandle m_PointClampSampler;
    nvrhi::SamplerHandle m_LinearClampSampler;
    nvrhi::SamplerHandle m_LinearWrapSampler;
    nvrhi::SamplerHandle m_AnisotropicWrapSampler;

    nvrhi::BindingLayoutHandle m_BlitBindingLayout;

private:
    rhi::FullscreenBlitPass* m_Blit = nullptr;
};

} // namespace caustica
