#include <render/passes/geometry/SkyPass.h>
#include <render/core/FramebufferFactory.h>
#include <backend/ViewRhiConversion.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <render/core/RenderDevice.h>
#include <scene/View.h>
#include <scene/SceneObjects.h>

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
    caustica::render::RenderDevice& renderDevice,
    const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView)
    : m_FramebufferFactory(framebufferFactory)
{
    m_PixelShader = shaderFactory->createAutoShader("engine/passes/sky_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_sky_ps), nullptr, nvrhi::ShaderType::Pixel);

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
        pipelineDesc.VS = sampleView->isReverseDepth() ? renderDevice.blit().fullscreenVS() : renderDevice.blit().fullscreenAtOneVS();
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

        nvrhi::FramebufferInfo framebufferInfo = m_FramebufferFactory->getFramebufferInfo();

        m_RenderPso = device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    }
}

void SkyPass::render(
    nvrhi::ICommandList* commandList,
    const ICompositeView& compositeView,
    const DirectionalLight& light,
    const SkyParameters& params) const
{
    commandList->beginMarker("Sky");

    for (uint viewIndex = 0; viewIndex < compositeView.getNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.getChildView(ViewType::PLANAR, viewIndex);

        nvrhi::GraphicsState state;
        state.pipeline = m_RenderPso;
        state.framebuffer = m_FramebufferFactory->getFramebuffer(*view);
        state.bindings = { m_RenderBindingSet };
        state.viewport = toNvrhi(view->getViewportState());
        
        dm::affine viewToWorld = view->getInverseViewMatrix();
        viewToWorld.m_translation = 0.f;
        dm::float4x4 clipToTranslatedWorld = view->getInverseProjectionMatrix(true) * affineToHomogeneous(viewToWorld);

        SkyConstants skyConstants{};
        skyConstants.matClipToTranslatedWorld = clipToTranslatedWorld;
        fillShaderParameters(light, params, skyConstants.params);
        commandList->writeBuffer(m_SkyCB, &skyConstants, sizeof(skyConstants));

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = 4;
        commandList->draw(args);
    }

    commandList->endMarker();
}

void SkyPass::fillShaderParameters(const caustica::DirectionalLight& light, const SkyParameters& input, ProceduralSkyShaderParameters& output)
{
    float lightAngularSize = dm::radians(clamp(light.angularSize, 0.1f, 90.f));
    float lightSolidAngle = 4 * dm::PI_f * square(sinf(lightAngularSize * 0.5f));
    float lightRadiance = light.irradiance / lightSolidAngle;
    if (input.maxLightRadiance > 0.f)
        lightRadiance = min(lightRadiance, input.maxLightRadiance);

    output.directionToLight = float3(normalize(-light.getDirection()));
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
