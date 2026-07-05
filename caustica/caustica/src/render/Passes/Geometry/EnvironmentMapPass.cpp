#include <render/Passes/Geometry/EnvironmentMapPass.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/RenderPassConstants.h>
#include <render/Core/RenderDevice.h>
#include <scene/View.h>
#include <math/math.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/environment_map_ps.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/environment_map_ps.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/environment_map_ps.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/sky_cb.h>

using namespace caustica;
using namespace caustica::render;

EnvironmentMapPass::EnvironmentMapPass(
    nvrhi::IDevice* device,
    std::shared_ptr<ShaderFactory> shaderFactory,
    caustica::render::RenderDevice& renderDevice,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView,
    nvrhi::ITexture* environmentMap)
    : m_renderDevice(&renderDevice)
    , m_FramebufferFactory(framebufferFactory)
{
    nvrhi::TextureDimension envMapDimension = environmentMap->getDesc().dimension;
    bool isCubeMap = (envMapDimension == nvrhi::TextureDimension::TextureCube) || 
        (envMapDimension == nvrhi::TextureDimension::TextureCubeArray);

    std::vector<caustica::ShaderMacro> PSMacros;
    PSMacros.push_back(caustica::ShaderMacro("LATLONG_TEXTURE", isCubeMap ? "0" : "1"));

    m_PixelShader = shaderFactory->CreateAutoShader("engine/passes/environment_map_ps.hlsl", "main", 
        CAUSTICA_MAKE_PLATFORM_SHADER(g_environment_map_ps), &PSMacros, nvrhi::ShaderType::Pixel);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(SkyConstants);
    constantBufferDesc.debugName = "SkyConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
    m_SkyCB = device->createBuffer(constantBufferDesc);

    const IView* sampleView = compositeView.getChildView(ViewType::PLANAR, 0);

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        m_RenderBindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_SkyCB),
            nvrhi::BindingSetItem::Texture_SRV(0, environmentMap),
            nvrhi::BindingSetItem::Sampler(0, renderDevice.samplers().linearWrap())
        };
        m_RenderBindingSet = device->createBindingSet(bindingSetDesc, m_RenderBindingLayout);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = sampleView->isReverseDepth() ? m_renderDevice->blit().fullscreenVS() : m_renderDevice->blit().fullscreenAtOneVS();
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_RenderBindingLayout };

        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState
            .enableDepthTest()
            .disableDepthWrite()
            .disableStencil()
            .setDepthFunc(sampleView->isReverseDepth()
                ? nvrhi::ComparisonFunc::GreaterOrEqual
                : nvrhi::ComparisonFunc::LessOrEqual);

        m_RenderPso = device->createGraphicsPipeline(pipelineDesc, m_FramebufferFactory->getFramebufferInfo());
    }
}

void EnvironmentMapPass::Render(
    nvrhi::ICommandList* commandList,
    const ICompositeView& compositeView)
{
    commandList->beginMarker("Environment Map");

    for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);

        nvrhi::GraphicsState state;
        state.pipeline = m_RenderPso;
        state.framebuffer = m_FramebufferFactory->getFramebuffer(*view);
        state.bindings = { m_RenderBindingSet };
        state.viewport = view->getViewportState();

        SkyConstants skyConstants = {};
        skyConstants.matClipToTranslatedWorld = view->getInverseViewProjectionMatrix() * affineToHomogeneous(translation(-view->getViewOrigin()));
        commandList->writeBuffer(m_SkyCB, &skyConstants, sizeof(skyConstants));

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = 4;
        commandList->draw(args);
    }

    commandList->endMarker();
}
