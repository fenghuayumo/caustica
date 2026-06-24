#include <render/SkyPass.h>
#include <render/DrawStrategy.h>
#include <render/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/ShadowMap.h>
#include <render/CommonRenderPasses.h>
#include <render/View.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/sky_ps.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/sky_ps.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/sky_ps.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/sky_cb.h>

using namespace caustica;
using namespace caustica::render;

SkyPass::SkyPass(
    nvrhi::IDevice* device,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses,
    const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView)
    : m_FramebufferFactory(framebufferFactory)
{
    m_PixelShader = shaderFactory->CreateAutoShader("caustica/passes/sky_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_sky_ps), nullptr, nvrhi::ShaderType::Pixel);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(SkyConstants);
    constantBufferDesc.debugName = "SkyConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
    m_SkyCB = device->createBuffer(constantBufferDesc);

    const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)
        };
        m_RenderBindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_SkyCB)
        };
        m_RenderBindingSet = device->createBindingSet(bindingSetDesc, m_RenderBindingLayout);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = sampleView->IsReverseDepth() ? commonPasses->m_FullscreenVS : commonPasses->m_FullscreenAtOneVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_RenderBindingLayout };

        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState
            .enableDepthTest()
            .disableDepthWrite()
            .disableStencil()
            .setDepthFunc(sampleView->IsReverseDepth()
                ? nvrhi::ComparisonFunc::GreaterOrEqual
                : nvrhi::ComparisonFunc::LessOrEqual);

        nvrhi::FramebufferInfo framebufferInfo = m_FramebufferFactory->GetFramebufferInfo();

        m_RenderPso = device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    }
}

void SkyPass::Render(
    nvrhi::ICommandList* commandList,
    const ICompositeView& compositeView,
    const DirectionalLight& light,
    const SkyParameters& params) const
{
    commandList->beginMarker("Sky");

    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

        nvrhi::GraphicsState state;
        state.pipeline = m_RenderPso;
        state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        state.bindings = { m_RenderBindingSet };
        state.viewport = view->GetViewportState();
        
        dm::affine viewToWorld = view->GetInverseViewMatrix();
        viewToWorld.m_translation = 0.f;
        dm::float4x4 clipToTranslatedWorld = view->GetInverseProjectionMatrix(true) * affineToHomogeneous(viewToWorld);

        SkyConstants skyConstants{};
        skyConstants.matClipToTranslatedWorld = clipToTranslatedWorld;
        FillShaderParameters(light, params, skyConstants.params);
        commandList->writeBuffer(m_SkyCB, &skyConstants, sizeof(skyConstants));

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = 4;
        commandList->draw(args);
    }

    commandList->endMarker();
}

void SkyPass::FillShaderParameters(const caustica::DirectionalLight& light, const SkyParameters& input, ProceduralSkyShaderParameters& output)
{
    float lightAngularSize = dm::radians(clamp(light.angularSize, 0.1f, 90.f));
    float lightSolidAngle = 4 * dm::PI_f * square(sinf(lightAngularSize * 0.5f));
    float lightRadiance = light.irradiance / lightSolidAngle;
    if (input.maxLightRadiance > 0.f)
        lightRadiance = min(lightRadiance, input.maxLightRadiance);

    output.directionToLight = float3(normalize(-light.GetDirection()));
    output.angularSizeOfLight = lightAngularSize;
    output.lightColor = lightRadiance * light.color;
    output.glowSize = dm::radians(dm::clamp(input.glowSize, 0.f, 90.f));
    output.skyColor = input.skyColor * input.brightness;
    output.glowIntensity = dm::clamp(input.glowIntensity, 0.f, 1.f);
    output.horizonColor = input.horizonColor * input.brightness;
    output.horizonSize = dm::radians(dm::clamp(input.horizonSize, 0.f, 90.f));
    output.groundColor = input.groundColor * input.brightness;
    output.glowSharpness = dm::clamp(input.glowSharpness, 1.f, 10.f);
    output.directionUp = normalize(input.directionUp);
}
