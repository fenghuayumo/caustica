#include <render/passes/geometry/GBufferFillPass.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/ShadowMap.h>
#include <scene/SceneTypes.h>
#include <render/core/RenderDevice.h>
#include <render/core/MaterialBindingCache.h>
#include <core/log.h>
#include <rhi/utils.h>
#include <utility>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/cubemap_gs.dxbc.h"
#include "compiled_shaders/passes/gbuffer_ps.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxbc.h"
#include "compiled_shaders/passes/material_id_ps.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/cubemap_gs.dxil.h"
#include "compiled_shaders/passes/gbuffer_ps.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxil.h"
#include "compiled_shaders/passes/material_id_ps.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/cubemap_gs.spirv.h"
#include "compiled_shaders/passes/gbuffer_ps.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.spirv.h"
#include "compiled_shaders/passes/material_id_ps.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/gbuffer_cb.h>

using namespace caustica;
using namespace caustica::render;

GBufferFillPass::GBufferFillPass(nvrhi::IDevice* device, caustica::render::RenderDevice& renderDevice)
    : m_device(device)
    , m_renderDevice(&renderDevice)
{
    m_IsDX11 = m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

void GBufferFillPass::init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_EnableMotionVectors = params.enableMotionVectors;
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.enableSinglePassCubemap)
        m_SupportedViewTypes = ViewType::Enum(m_SupportedViewTypes | ViewType::CUBEMAP);
    
    m_VertexShader = createVertexShader(shaderFactory, params);
    m_InputLayout = createInputLayout(m_VertexShader, params);
    m_GeometryShader = createGeometryShader(shaderFactory, params);
    m_PixelShader = createPixelShader(shaderFactory, params, false);
    m_PixelShaderAlphaTested = createPixelShader(shaderFactory, params, true);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = createMaterialBindingCache(*m_renderDevice);

    m_GBufferCB = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GBufferFillConstants),
        "GBufferFillConstants", params.numConstantBufferVersions));

    createViewBindings(m_ViewBindingLayout, m_ViewBindings, params);

    m_EnableDepthWrite = params.enableDepthWrite;
    m_StencilWriteMask = params.stencilWriteMask;

    m_InputBindingLayout = createInputBindingLayout();
}

void GBufferFillPass::resetBindingCache()
{
    m_MaterialBindings->clear();
    m_InputBindingSets.clear();
}

nvrhi::ShaderHandle GBufferFillPass::createVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "engine/passes/gbuffer_vs.hlsl";

    std::vector<ShaderMacro> VertexShaderMacros;
    VertexShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));

    if (params.useInputAssembler)
    {
        return shaderFactory.createAutoShader(sourceFileName, "input_assembler",
            CAUSTICA_MAKE_PLATFORM_SHADER(g_gbuffer_vs_input_assembler), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.createAutoShader(sourceFileName, "buffer_loads",
            CAUSTICA_MAKE_PLATFORM_SHADER(g_gbuffer_vs_buffer_loads), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
}

nvrhi::ShaderHandle GBufferFillPass::createGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{

    ShaderMacro MotionVectorsMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0");

    if (params.enableSinglePassCubemap)
    {
        // MVs will not work with cubemap views because:
        // 1. cubemap_gs does not pass through the previous position attribute;
        // 2. Computing correct MVs for a cubemap is complicated and not implemented.
        assert(!params.enableMotionVectors);

        auto desc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags(
                nvrhi::FastGeometryShaderFlags::ForceFastGS |
                nvrhi::FastGeometryShaderFlags::UseViewportMask |
                nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex))
            .setCoordinateSwizzling(CubemapView::getCubemapCoordinateSwizzle());

        return shaderFactory.createAutoShader("engine/passes/cubemap_gs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_cubemap_gs), nullptr, desc);
    }
    else
    {
        return nullptr;
    }
}

nvrhi::ShaderHandle GBufferFillPass::createPixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));

    return shaderFactory.createAutoShader("engine/passes/gbuffer_ps.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_gbuffer_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle GBufferFillPass::createInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    if (params.useInputAssembler)
    {
        std::vector<nvrhi::VertexAttributeDesc> inputDescs =
        {
            getVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
            getVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
            getVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 2),
            getVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 3),
            getVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 4),
            getVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 5),
        };
        if (params.enableMotionVectors)
        {
            inputDescs.push_back(getVertexAttributeDesc(VertexAttribute::PrevTransform, "PREV_TRANSFORM", 5));
        }

        return m_device->createInputLayout(inputDescs.data(), static_cast<uint32_t>(inputDescs.size()), vertexShader);
    }

    return nullptr;
}

void GBufferFillPass::createViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(GBUFFER_SPACE_VIEW)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER));

    layout = m_device->createBindingLayout(bindingLayoutDesc);

    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(params.trackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS, m_GBufferCB))
        .addItem(nvrhi::BindingSetItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER,
            m_renderDevice->samplers().anisotropicWrap()));

    set = m_device->createBindingSet(bindingSetDesc, layout);
}

nvrhi::GraphicsPipelineHandle GBufferFillPass::createGraphicsPipeline(PipelineKey key, nvrhi::FramebufferInfo const& framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.VS = m_VertexShader;
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.renderState.rasterState
        .setFrontCounterClockwise(key.bits.frontCounterClockwise)
        .setCullMode(key.bits.cullMode);
    pipelineDesc.renderState.blendState.disableAlphaToCoverage();
    pipelineDesc.bindingLayouts = { m_MaterialBindings->getLayout(), m_ViewBindingLayout };
    if (!m_UseInputAssembler)
        pipelineDesc.bindingLayouts.push_back(m_InputBindingLayout);

    pipelineDesc.renderState.depthStencilState
        .setDepthWriteEnable(m_EnableDepthWrite)
        .setDepthFunc(key.bits.reverseDepth
            ? nvrhi::ComparisonFunc::GreaterOrEqual
            : nvrhi::ComparisonFunc::LessOrEqual);
        
    if (m_StencilWriteMask)
    {
        pipelineDesc.renderState.depthStencilState
            .enableStencil()
            .setStencilReadMask(0)
            .setStencilWriteMask(uint8_t(m_StencilWriteMask))
            .setStencilRefValue(uint8_t(m_StencilWriteMask))
            .setFrontFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace))
            .setBackFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace));
    }

    if (key.bits.alphaTested)
    {
        pipelineDesc.renderState.rasterState.setCullNone();

        if (m_PixelShaderAlphaTested)
        {
            pipelineDesc.PS = m_PixelShaderAlphaTested;
        }
        else
        {
            pipelineDesc.PS = m_PixelShader;
            pipelineDesc.renderState.blendState.alphaToCoverageEnable = true;
        }
    }
    else
    {
        pipelineDesc.PS = m_PixelShader;
    }

    return m_device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
}

std::shared_ptr<MaterialBindingCache> GBufferFillPass::createMaterialBindingCache(caustica::render::RenderDevice& renderDevice)
{
    std::vector<MaterialResourceBinding> materialBindings = {
        { MaterialResource::ConstantBuffer,         GBUFFER_BINDING_MATERIAL_CONSTANTS },
        { MaterialResource::DiffuseTexture,         GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResource::SpecularTexture,        GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE },
        { MaterialResource::normalTexture,          GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE },
        { MaterialResource::emissiveTexture,        GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        { MaterialResource::OcclusionTexture,       GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE },
        { MaterialResource::transmissionTexture,    GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE },
        { MaterialResource::OpacityTexture,         GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE }
    };

    return std::make_shared<MaterialBindingCache>(
        m_device,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ GBUFFER_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ true,
        materialBindings,
        renderDevice.samplers().anisotropicWrap(),
        renderDevice.builtins().grayTexture(),
        renderDevice.builtins().blackTexture());
}

ViewType::Enum GBufferFillPass::getSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

void GBufferFillPass::setupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    GBufferFillConstants gbufferConstants = {};
    view->fillPlanarViewConstants(gbufferConstants.view);
    viewPrev->fillPlanarViewConstants(gbufferConstants.viewPrev);
    commandList->writeBuffer(m_GBufferCB, &gbufferConstants, sizeof(gbufferConstants));

    context.keyTemplate.bits.frontCounterClockwise = view->isMirrored();
    context.keyTemplate.bits.reverseDepth = view->isReverseDepth();
}

bool GBufferFillPass::setupMaterial(GeometryPassContext& abstractContext, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    PipelineKey key = context.keyTemplate;
    key.bits.cullMode = cullMode;

    switch (material->domain)
    {
    case MaterialDomain::Opaque:
    case MaterialDomain::AlphaBlended: // Blended and transmissive domains are for the material ID pass, shouldn't be used otherwise
    case MaterialDomain::Transmissive:
    case MaterialDomain::TransmissiveAlphaTested:
    case MaterialDomain::TransmissiveAlphaBlended:
        key.bits.alphaTested = false;
        break;
    case MaterialDomain::AlphaTested:
        key.bits.alphaTested = true;
        break;
    default:
        return false;
    }

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->getMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

    nvrhi::FramebufferInfo const& framebufferInfo = state.framebuffer->getFramebufferInfo();
    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key.value];

    if (!pipeline)
    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);

        if (!pipeline)
            pipeline = createGraphicsPipeline(key, framebufferInfo);

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == framebufferInfo);

    state.pipeline = pipeline;
    state.bindings = { materialBindingSet, m_ViewBindings };
    
    if (!m_UseInputAssembler)
        state.bindings.push_back(context.inputBindingSet);

    return true;
}

void GBufferFillPass::setupInputBuffers(GeometryPassContext& abstractContext, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state)
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
        context.prevPositionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

nvrhi::BindingLayoutHandle GBufferFillPass::createInputBindingLayout()
{
    if (m_UseInputAssembler)
        return nullptr;

    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpaceAndDescriptorSet(GBUFFER_SPACE_INPUT)
        .addItem(m_IsDX11
            ? nvrhi::BindingLayoutItem::RawBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER)
            : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(GBUFFER_BINDING_VERTEX_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(GBUFFER_BINDING_PUSH_CONSTANTS, sizeof(GBufferPushConstants)));
        
    return m_device->createBindingLayout(bindingLayoutDesc);
}

nvrhi::BindingSetHandle GBufferFillPass::createInputBindingSet(const BufferGroup* bufferGroup)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .addItem(m_IsDX11
            ? nvrhi::BindingSetItem::RawBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer)
            : nvrhi::BindingSetItem::StructuredBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
        .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(GBUFFER_BINDING_VERTEX_BUFFER, bufferGroup->vertexBuffer))
        .addItem(nvrhi::BindingSetItem::PushConstants(GBUFFER_BINDING_PUSH_CONSTANTS, sizeof(GBufferPushConstants)));

    return m_device->createBindingSet(bindingSetDesc, m_InputBindingLayout);
}

nvrhi::BindingSetHandle GBufferFillPass::getOrCreateInputBindingSet(const BufferGroup* bufferGroup)
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

void GBufferFillPass::setPushConstants(
    caustica::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    if (m_UseInputAssembler)
        return;
        
    auto& context = static_cast<Context&>(abstractContext);

    GBufferPushConstants constants;
    constants.startInstanceLocation = args.startInstanceLocation;
    constants.startVertexLocation = args.startVertexLocation;
    constants.positionOffset = context.positionOffset;
    constants.prevPositionOffset = context.prevPositionOffset;
    constants.texCoordOffset = context.texCoordOffset;
    constants.normalOffset = context.normalOffset;
    constants.tangentOffset = context.tangentOffset;

    commandList->setPushConstants(&constants, sizeof(constants));

    args.startInstanceLocation = 0;
    args.startVertexLocation = 0;
}

void MaterialIDPass::init(
    caustica::ShaderFactory& shaderFactory,
    const CreateParameters& params)
{
    CreateParameters paramsCopy = params;
    // The material ID pass relies on the push constants filled by the buffer load path (firstInstance)
    paramsCopy.useInputAssembler = false;
    // The material ID pass doesn't support generating motion vectors
    paramsCopy.enableMotionVectors = false;

    GBufferFillPass::init(shaderFactory, paramsCopy);
}

nvrhi::ShaderHandle MaterialIDPass::createPixelShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));

    return shaderFactory.createAutoShader("engine/passes/material_id_ps.hlsl", "main",
        CAUSTICA_MAKE_PLATFORM_SHADER(g_material_id_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}
