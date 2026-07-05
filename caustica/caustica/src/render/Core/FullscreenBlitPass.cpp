#include <render/Core/FullscreenBlitPass.h>

#include <assets/loader/ShaderFactory.h>
#include <render/Core/BindingCache.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/fullscreen_vs.dxbc.h"
#include "compiled_shaders/rect_vs.dxbc.h"
#include "compiled_shaders/blit_ps.dxbc.h"
#include "compiled_shaders/sharpen_ps.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/fullscreen_vs.dxil.h"
#include "compiled_shaders/rect_vs.dxil.h"
#include "compiled_shaders/blit_ps.dxil.h"
#include "compiled_shaders/sharpen_ps.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/fullscreen_vs.spirv.h"
#include "compiled_shaders/rect_vs.spirv.h"
#include "compiled_shaders/blit_ps.spirv.h"
#include "compiled_shaders/sharpen_ps.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/blit_cb.h>

namespace caustica::render
{

FullscreenBlitPass::FullscreenBlitPass(nvrhi::IDevice* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory,
    const StandardSamplers& samplers)
    : m_Device(device)
    , m_Samplers(&samplers)
{
    {
        std::vector<ShaderMacro> vsMacros;
        vsMacros.push_back(ShaderMacro("QUAD_Z", "0"));
        m_FullscreenVS = shaderFactory->CreateAutoShader("engine/fullscreen_vs", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_fullscreen_vs), &vsMacros, nvrhi::ShaderType::Vertex);

        vsMacros[0].definition = "1";
        m_FullscreenAtOneVS = shaderFactory->CreateAutoShader("engine/fullscreen_vs", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_fullscreen_vs), &vsMacros, nvrhi::ShaderType::Vertex);
    }

    m_RectVS = shaderFactory->CreateAutoShader("engine/rect_vs", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_rect_vs), nullptr, nvrhi::ShaderType::Vertex);

    std::vector<ShaderMacro> blitMacros = { ShaderMacro("TEXTURE_ARRAY", "0") };
    m_BlitPS = shaderFactory->CreateAutoShader("engine/blit_ps", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_blit_ps), &blitMacros, nvrhi::ShaderType::Pixel);
    m_SharpenPS = shaderFactory->CreateAutoShader("engine/sharpen_ps", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_sharpen_ps), &blitMacros, nvrhi::ShaderType::Pixel);
    blitMacros[0].definition = "1";
    m_BlitArrayPS = shaderFactory->CreateAutoShader("engine/blit_ps", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_blit_ps), &blitMacros, nvrhi::ShaderType::Pixel);
    m_SharpenArrayPS = shaderFactory->CreateAutoShader("engine/sharpen_ps", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_sharpen_ps), &blitMacros, nvrhi::ShaderType::Pixel);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BlitConstants)),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0)
    };
    m_BlitBindingLayout = m_Device->createBindingLayout(layoutDesc);
}

#ifdef _DEBUG
static bool IsSupportedBlitDimension(nvrhi::TextureDimension dimension)
{
    return dimension == nvrhi::TextureDimension::Texture2D
        || dimension == nvrhi::TextureDimension::Texture2DArray
        || dimension == nvrhi::TextureDimension::TextureCube
        || dimension == nvrhi::TextureDimension::TextureCubeArray;
}
#endif

static bool IsTextureArray(nvrhi::TextureDimension dimension)
{
    return dimension == nvrhi::TextureDimension::Texture2DArray
        || dimension == nvrhi::TextureDimension::TextureCube
        || dimension == nvrhi::TextureDimension::TextureCubeArray;
}

void FullscreenBlitPass::blitTexture(nvrhi::ICommandList* commandList, const BlitParameters& params, caustica::BindingCache* bindingCache)
{
    assert(commandList);
    assert(params.targetFramebuffer);
    assert(params.sourceTexture);
    assert(m_Samplers);

#ifdef _DEBUG
    const nvrhi::FramebufferDesc& targetFramebufferDesc = params.targetFramebuffer->getDesc();
    assert(targetFramebufferDesc.colorAttachments.size() == 1);
    assert(targetFramebufferDesc.colorAttachments[0].valid());
#endif

    const nvrhi::FramebufferInfoEx& fbinfo = params.targetFramebuffer->getFramebufferInfo();
    const nvrhi::TextureDesc& sourceDesc = params.sourceTexture->getDesc();

#ifdef _DEBUG
    assert(IsSupportedBlitDimension(sourceDesc.dimension));
#endif
    const bool isTextureArray = IsTextureArray(sourceDesc.dimension);

    nvrhi::Viewport targetViewport = params.targetViewport;
    if (targetViewport.width() == 0 && targetViewport.height() == 0)
        targetViewport = nvrhi::Viewport(float(fbinfo.width), float(fbinfo.height));

    nvrhi::IShader* shader = nullptr;
    switch (params.sampler)
    {
    case BlitSampler::Point:
    case BlitSampler::Linear: shader = isTextureArray ? m_BlitArrayPS : m_BlitPS; break;
    case BlitSampler::Sharpen: shader = isTextureArray ? m_SharpenArrayPS : m_SharpenPS; break;
    default: assert(false);
    }

    nvrhi::GraphicsPipelineHandle& pso = m_BlitPsoCache[PsoCacheKey{ fbinfo, shader, params.blendState }];
    if (!pso)
    {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.bindingLayouts = { m_BlitBindingLayout };
        psoDesc.VS = m_RectVS;
        psoDesc.PS = shader;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.renderState.rasterState.setCullNone();
        psoDesc.renderState.depthStencilState.depthTestEnable = false;
        psoDesc.renderState.depthStencilState.stencilEnable = false;
        psoDesc.renderState.blendState.targets[0] = params.blendState;
        pso = m_Device->createGraphicsPipeline(psoDesc, fbinfo);
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    {
        auto sourceDimension = sourceDesc.dimension;
        if (sourceDimension == nvrhi::TextureDimension::TextureCube || sourceDimension == nvrhi::TextureDimension::TextureCubeArray)
            sourceDimension = nvrhi::TextureDimension::Texture2DArray;

        const auto sourceSubresources = nvrhi::TextureSubresourceSet(params.sourceMip, 1, params.sourceArraySlice, 1);

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(BlitConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0, params.sourceTexture, params.sourceFormat, sourceSubresources, sourceDimension),
            nvrhi::BindingSetItem::Sampler(0, params.sampler == BlitSampler::Point ? m_Samplers->pointClamp() : m_Samplers->linearClamp())
        };
    }

    nvrhi::BindingSetHandle sourceBindingSet;
    if (bindingCache)
        sourceBindingSet = bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_BlitBindingLayout);
    else
        sourceBindingSet = m_Device->createBindingSet(bindingSetDesc, m_BlitBindingLayout);

    nvrhi::GraphicsState state;
    state.pipeline = pso;
    state.framebuffer = params.targetFramebuffer;
    state.bindings = { sourceBindingSet };
    state.viewport.addViewport(targetViewport);
    state.viewport.addScissorRect(nvrhi::Rect(targetViewport));
    state.blendConstantColor = params.blendConstantColor;

    BlitConstants blitConstants = {};
    blitConstants.sourceOrigin = float2(params.sourceBox.m_mins);
    blitConstants.sourceSize = params.sourceBox.diagonal();
    blitConstants.targetOrigin = float2(params.targetBox.m_mins);
    blitConstants.targetSize = params.targetBox.diagonal();

    commandList->setGraphicsState(state);
    commandList->setPushConstants(&blitConstants, sizeof(blitConstants));

    nvrhi::DrawArguments args;
    args.instanceCount = 1;
    args.vertexCount = 4;
    commandList->draw(args);
}

void FullscreenBlitPass::blitTexture(nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* targetFramebuffer,
    nvrhi::ITexture* sourceTexture,
    caustica::BindingCache* bindingCache)
{
    assert(commandList);
    assert(targetFramebuffer);
    assert(sourceTexture);

    BlitParameters params;
    params.targetFramebuffer = targetFramebuffer;
    params.sourceTexture = sourceTexture;
    blitTexture(commandList, params, bindingCache);
}

} // namespace caustica::render
