#include <render/passes/geometry/ForwardShadingPass.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <scene/Scene.h>
#include <scene/SceneTypes.h>
#include <scene/SceneObjects.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneLightAccess.h>
#include <render/core/RenderDevice.h>
#include <render/core/MaterialBindingCache.h>
#include <core/log.h>
#include <rhi/utils.h>
#include <utility>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/cubemap_gs.dxbc.h"
#include "compiled_shaders/passes/forward_ps.dxbc.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/cubemap_gs.dxil.h"
#include "compiled_shaders/passes/forward_ps.dxil.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/cubemap_gs.spirv.h"
#include "compiled_shaders/passes/forward_ps.spirv.h"
#include "compiled_shaders/passes/forward_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/forward_vs_buffer_loads.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/forward_cb.h>


using namespace caustica;
using namespace caustica::render;

ForwardShadingPass::ForwardShadingPass(
    nvrhi::IDevice* device,
    caustica::render::RenderDevice& renderDevice)
    : m_device(device)
    , m_renderDevice(&renderDevice)
{
    m_IsDX11 = m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

void ForwardShadingPass::init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.singlePassCubemap)
        m_SupportedViewTypes = ViewType::CUBEMAP;
    
    m_VertexShader = createVertexShader(shaderFactory, params);
    m_InputLayout = createInputLayout(m_VertexShader, params);
    m_GeometryShader = createGeometryShader(shaderFactory, params);
    m_PixelShader = createPixelShader(shaderFactory, params, false);
    m_PixelShaderTransmissive = createPixelShader(shaderFactory, params, true);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = createMaterialBindingCache(*m_renderDevice);

    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
        .setBorderColor(1.0f);
    m_ShadowSampler = m_device->createSampler(samplerDesc);

    m_ForwardViewCB = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ForwardShadingViewConstants), "ForwardShadingViewConstants", params.numConstantBufferVersions));
    m_ForwardLightCB = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ForwardShadingLightConstants), "ForwardShadingLightConstants", params.numConstantBufferVersions));

    m_ViewBindingLayout = createViewBindingLayout();
    m_ViewBindingSet = createViewBindingSet();
    m_ShadingBindingLayout = createShadingBindingLayout();
    m_InputBindingLayout = createInputBindingLayout();
}

void ForwardShadingPass::resetBindingCache()
{
    m_MaterialBindings->clear();
    m_ShadingBindingSets.clear();
    m_InputBindingSets.clear();
}

nvrhi::ShaderHandle ForwardShadingPass::createVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "engine/passes/forward_vs.hlsl";

    if (params.useInputAssembler)
    {
        return shaderFactory.createAutoShader(sourceFileName, "input_assembler",
            CAUSTICA_MAKE_PLATFORM_SHADER(g_forward_vs_input_assembler), nullptr, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.createAutoShader(sourceFileName, "buffer_loads",
            CAUSTICA_MAKE_PLATFORM_SHADER(g_forward_vs_buffer_loads), nullptr, nvrhi::ShaderType::Vertex);
    }
}

nvrhi::ShaderHandle ForwardShadingPass::createGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    if (params.singlePassCubemap)
    {
        auto desc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags(
                nvrhi::FastGeometryShaderFlags::ForceFastGS |
                nvrhi::FastGeometryShaderFlags::UseViewportMask |
                nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex))
            .setCoordinateSwizzling(CubemapView::getCubemapCoordinateSwizzle());

        return shaderFactory.createAutoShader("engine/passes/cubemap_gs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_cubemap_gs), nullptr, desc);
    }

    return nullptr;
}

nvrhi::ShaderHandle ForwardShadingPass::createPixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial)
{
    std::vector<ShaderMacro> Macros;
    Macros.push_back(ShaderMacro("TRANSMISSIVE_MATERIAL", transmissiveMaterial ? "1" : "0"));

    return shaderFactory.createAutoShader("engine/passes/forward_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_forward_ps), &Macros, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle ForwardShadingPass::createInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    if (params.useInputAssembler)
    {
        const nvrhi::VertexAttributeDesc inputDescs[] =
        {
            getVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
            getVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
            getVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 2),
            getVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 3),
            getVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 4),
            getVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 5),
        };

        return m_device->createInputLayout(inputDescs, uint32_t(std::size(inputDescs)), vertexShader);
    }
    
    return nullptr;
}

nvrhi::BindingLayoutHandle ForwardShadingPass::createViewBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS));

    return m_device->createBindingLayout(bindingLayoutDesc);
}


nvrhi::BindingSetHandle ForwardShadingPass::createViewBindingSet()
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(m_TrackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_VIEW_CONSTANTS, m_ForwardViewCB));

    return m_device->createBindingSet(bindingSetDesc, m_ViewBindingLayout);
}

nvrhi::BindingLayoutHandle ForwardShadingPass::createShadingBindingLayout()
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_SHADING)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(FORWARD_BINDING_LIGHT_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_MATERIAL_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER));

    return m_device->createBindingLayout(bindingLayoutDesc);
}

nvrhi::BindingSetHandle ForwardShadingPass::createShadingBindingSet(nvrhi::ITexture* shadowMapTexture,
    nvrhi::ITexture* diffuse, nvrhi::ITexture* specular, nvrhi::ITexture* environmentBrdf)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(m_TrackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_LIGHT_CONSTANTS, m_ForwardLightCB))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE,
            shadowMapTexture ? shadowMapTexture : m_renderDevice->builtins().blackTexture2DArray().Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE,
            diffuse ? diffuse : m_renderDevice->builtins().blackCubeMapArray().Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE,
            specular ? specular : m_renderDevice->builtins().blackCubeMapArray().Get()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE,
            environmentBrdf ? environmentBrdf : m_renderDevice->builtins().blackTexture().Get()))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_MATERIAL_SAMPLER,
            m_renderDevice->samplers().anisotropicWrap()))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER,
            m_ShadowSampler))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER,
            m_renderDevice->samplers().linearWrap()))
        .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER,
            m_renderDevice->samplers().linearClamp()));

    return m_device->createBindingSet(bindingSetDesc, m_ShadingBindingLayout);
}


nvrhi::GraphicsPipelineHandle ForwardShadingPass::createGraphicsPipeline(ForwardShadingPassPipelineKey const& key,
    nvrhi::FramebufferInfo const& framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.VS = m_VertexShader;
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.renderState.rasterState.frontCounterClockwise = key.frontCounterClockwise;
    pipelineDesc.renderState.rasterState.setCullMode(key.cullMode);
    pipelineDesc.renderState.blendState.alphaToCoverageEnable = false;
    pipelineDesc.shadingRateState = key.shadingRateState;
    pipelineDesc.bindingLayouts = { m_MaterialBindings->getLayout(), m_ViewBindingLayout, m_ShadingBindingLayout };
    if (!m_UseInputAssembler)
        pipelineDesc.bindingLayouts.push_back(m_InputBindingLayout);

    bool const framebufferUsesMSAA = framebufferInfo.sampleCount > 1;

    pipelineDesc.renderState.depthStencilState
        .setDepthFunc(key.reverseDepth
            ? nvrhi::ComparisonFunc::GreaterOrEqual
            : nvrhi::ComparisonFunc::LessOrEqual);
    
    switch (key.domain)
    {
    case MaterialDomain::Opaque:
        pipelineDesc.PS = m_PixelShader;
        break;

    case MaterialDomain::AlphaTested:
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.renderState.blendState.alphaToCoverageEnable = framebufferUsesMSAA;
        break;

    case MaterialDomain::AlphaBlended: {
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.renderState.blendState.targets[0]
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
        
        pipelineDesc.renderState.depthStencilState.disableDepthWrite();
        break;
    }

    case MaterialDomain::Transmissive:
    case MaterialDomain::TransmissiveAlphaTested:
    case MaterialDomain::TransmissiveAlphaBlended: {
        pipelineDesc.PS = m_PixelShaderTransmissive;
        pipelineDesc.renderState.blendState.targets[0]
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::One)
            .setDestBlend(nvrhi::BlendFactor::Src1Color)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);

        pipelineDesc.renderState.depthStencilState.disableDepthWrite();
        break;
    }
    default:
        return nullptr;
    }

    return m_device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

std::shared_ptr<MaterialBindingCache> ForwardShadingPass::createMaterialBindingCache(caustica::render::RenderDevice& renderDevice)
{
    std::vector<MaterialResourceBinding> materialBindings = {
        { MaterialResource::ConstantBuffer,         FORWARD_BINDING_MATERIAL_CONSTANTS },
        { MaterialResource::DiffuseTexture,         FORWARD_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResource::SpecularTexture,        FORWARD_BINDING_MATERIAL_SPECULAR_TEXTURE },
        { MaterialResource::normalTexture,          FORWARD_BINDING_MATERIAL_NORMAL_TEXTURE },
        { MaterialResource::emissiveTexture,        FORWARD_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        { MaterialResource::OcclusionTexture,       FORWARD_BINDING_MATERIAL_OCCLUSION_TEXTURE },
        { MaterialResource::transmissionTexture,    FORWARD_BINDING_MATERIAL_TRANSMISSION_TEXTURE },
        { MaterialResource::OpacityTexture,         FORWARD_BINDING_MATERIAL_OPACITY_TEXTURE }
    };

    return std::make_shared<MaterialBindingCache>(
        m_device,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ FORWARD_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ true,
        materialBindings,
        renderDevice.samplers().anisotropicWrap(),
        renderDevice.builtins().grayTexture(),
        renderDevice.builtins().blackTexture());
}

void ForwardShadingPass::setupView(
    GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    const IView* view,
    const IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    ForwardShadingViewConstants viewConstants = {};
    view->fillPlanarViewConstants(viewConstants.view);
    commandList->writeBuffer(m_ForwardViewCB, &viewConstants, sizeof(viewConstants));

    context.keyTemplate.frontCounterClockwise = view->isMirrored();
    context.keyTemplate.reverseDepth = view->isReverseDepth();
}

void ForwardShadingPass::prepareLights(
    Context& context,
    nvrhi::ICommandList* commandList,
    const caustica::Scene& scene,
    dm::float3 ambientColorTop,
    dm::float3 ambientColorBottom,
    const std::vector<std::shared_ptr<LightProbe>>& lightProbes)
{
    nvrhi::ITexture* shadowMapTexture = nullptr;
    int2 shadowMapTextureSize = 0;
    const auto& renderLights = scene.getRenderData().lights;

    nvrhi::ITexture* lightProbeDiffuse = nullptr;
    nvrhi::ITexture* lightProbeSpecular = nullptr;
    nvrhi::ITexture* lightProbeEnvironmentBrdf = nullptr;

    for (const auto& probe : lightProbes)
    {
        if (!probe->enabled)
            continue;

        if (lightProbeDiffuse == nullptr || lightProbeSpecular == nullptr || lightProbeEnvironmentBrdf == nullptr)
        {
            lightProbeDiffuse = probe->diffuseMap;
            lightProbeSpecular = probe->specularMap;
            lightProbeEnvironmentBrdf = probe->environmentBrdf;
        }
        else
        {
            if (lightProbeDiffuse != probe->diffuseMap || lightProbeSpecular != probe->specularMap || lightProbeEnvironmentBrdf != probe->environmentBrdf)
            {
                caustica::error("All lights probe submitted to ForwardShadingPass::prepareLights(...) must use the same set of textures");
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);

        nvrhi::BindingSetHandle& shadingBindings = m_ShadingBindingSets[std::make_pair(shadowMapTexture, lightProbeDiffuse)];

        if (!shadingBindings)
        {
            shadingBindings = createShadingBindingSet(shadowMapTexture, lightProbeDiffuse, lightProbeSpecular, lightProbeEnvironmentBrdf);
        }

        context.shadingBindingSet = shadingBindings;
    }

    ForwardShadingLightConstants constants = {};

    constants.shadowMapTextureSize = float2(shadowMapTextureSize);
    constants.shadowMapTextureSizeInv = 1.f / constants.shadowMapTextureSize;

    {
        const int maxLights = std::min(static_cast<int>(renderLights.size()), FORWARD_MAX_LIGHTS);
        for (int nLight = 0; nLight < maxLights; nLight++)
        {
            const scene::LightRenderProxy& lightProxy = renderLights[nLight];
            LightConstants& lightConstants = constants.lights[constants.numLights];
            scene::fillLightConstants(lightProxy, lightConstants);
            ++constants.numLights;
        }
    }

    constants.ambientColorTop = float4(ambientColorTop, 0.f);
    constants.ambientColorBottom = float4(ambientColorBottom, 0.f);

    for (const auto& probe : lightProbes)
    {
        if (!probe->isActive())
            continue;

        LightProbeConstants& lightProbeConstants = constants.lightProbes[constants.numLightProbes];
        probe->fillLightProbeConstants(lightProbeConstants);

        ++constants.numLightProbes;

        if (constants.numLightProbes >= FORWARD_MAX_LIGHT_PROBES)
            break;
    }

    commandList->writeBuffer(m_ForwardLightCB, &constants, sizeof(constants));
}

ViewType::Enum ForwardShadingPass::getSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

bool ForwardShadingPass::setupMaterial(GeometryPassContext& abstractContext, const Material* material,
    nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->getMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

    if (material->domain >= MaterialDomain::Count || cullMode > nvrhi::RasterCullMode::None)
    {
        assert(false);
        return false;
    }

    ForwardShadingPassPipelineKey key = context.keyTemplate;
    key.cullMode = cullMode;
    key.domain = material->domain;

    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key];

    if (!pipeline)
    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);

        if (!pipeline)
            pipeline = createGraphicsPipeline(key, state.framebuffer->getFramebufferInfo());

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == state.framebuffer->getFramebufferInfo());

    state.pipeline = pipeline;
    state.bindings = { materialBindingSet, m_ViewBindingSet, context.shadingBindingSet };
    
    if (!m_UseInputAssembler)
        state.bindings.push_back(context.inputBindingSet);

    return true;
}

void ForwardShadingPass::setupInputBuffers(GeometryPassContext& abstractContext, const BufferGroup* buffers, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    state.indexBuffer = { buffers->indexBuffer, nvrhi::Format::R32_UINT, 0 };
    
    if (m_UseInputAssembler)
    {
        state.vertexBuffers = {
            { buffers->vertexBuffer, 0, buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset },
            { buffers->vertexBuffer, 1, buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset },
            { buffers->vertexBuffer, 2, buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset },
            { buffers->vertexBuffer, 3, buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset },
            { buffers->vertexBuffer, 4, buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset },
            { buffers->instanceBuffer, 5, 0 }
        };
    }
    else
    {
        context.inputBindingSet = getOrCreateInputBindingSet(buffers);
        context.positionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

nvrhi::BindingLayoutHandle ForwardShadingPass::createInputBindingLayout()
{
    if (m_UseInputAssembler)
        return nullptr;

    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex)
        .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_INPUT)
        .addItem(m_IsDX11
            ? nvrhi::BindingLayoutItem::RawBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER)
            : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS, sizeof(ForwardPushConstants)));
        
    return m_device->createBindingLayout(bindingLayoutDesc);
}

nvrhi::BindingSetHandle ForwardShadingPass::createInputBindingSet(const BufferGroup* bufferGroup)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .addItem(m_IsDX11
            ? nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer)
            : nvrhi::BindingSetItem::StructuredBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
        .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER, bufferGroup->vertexBuffer))
        .addItem(nvrhi::BindingSetItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS, sizeof(ForwardPushConstants)));

    return m_device->createBindingSet(bindingSetDesc, m_InputBindingLayout);
}

nvrhi::BindingSetHandle ForwardShadingPass::getOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    auto it = m_InputBindingSets.find(bufferGroup);
    if (it == m_InputBindingSets.end())
    {
        auto bindingSet = createInputBindingSet(bufferGroup);
        m_InputBindingSets[bufferGroup] = bindingSet;
        return bindingSet;
    }

    return it->second;
}

void ForwardShadingPass::setPushConstants(
    caustica::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    if (m_UseInputAssembler)
        return;
        
    auto& context = static_cast<Context&>(abstractContext);

    ForwardPushConstants constants;
    constants.startInstanceLocation = args.startInstanceLocation;
    constants.startVertexLocation = args.startVertexLocation;
    constants.positionOffset = context.positionOffset;
    constants.texCoordOffset = context.texCoordOffset;
    constants.normalOffset = context.normalOffset;
    constants.tangentOffset = context.tangentOffset;

    commandList->setPushConstants(&constants, sizeof(constants));

    args.startInstanceLocation = 0;
    args.startVertexLocation = 0;
}