//
// Author(s):  Filip Strugar (filip.strugar@intel.com)
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <rhi/utils.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <render/passes/postProcess/DenoisingGuidesPass.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>
#include <imgui/imgui_renderer.h>

using namespace caustica::math;

#include <shaders/render/processingPasses/DenoisingGuidesPass.hlsl>

#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include <render/core/RenderTargets.h>

#include <shaders/SampleConstantBuffer.h>

static_assert( sizeof(DenoisingGuidesPassConstants) == sizeof(SampleMiniConstants) );

DenoisingGuidesPass::DenoisingGuidesPass( caustica::rhi::Device* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory, const std::unique_ptr<RenderTargets> & renderTargets, const std::shared_ptr<ShaderDebug> & shaderDebug, caustica::rhi::BindingLayoutHandle bindingLayout )
    : m_device(device)
    , m_bindingCache(device)
    , m_renderTargets(renderTargets)
    , m_shaderDebug(shaderDebug)
    , m_bindingLayout(bindingLayout)
{ 

    // caustica::rhi::BindingLayoutDesc layoutDesc;
    // layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    // layoutDesc.bindings = { 
    //     caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
    //     caustica::rhi::BindingLayoutItem::Texture_UAV(0) };
    // m_bindingLayout = m_device->createBindingLayout(layoutDesc);

    caustica::rhi::ComputePipelineDesc pipelineDesc;

    // These need to know about the scene
    pipelineDesc.bindingLayouts = { m_bindingLayout };
    m_csDenoiseSpecHitT.init(m_device, *shaderFactory, "caustica/shaders/render/processingPasses/DenoisingGuidesPass.hlsl", "denoiseSpecHitT", std::vector<caustica::ShaderMacro>(), pipelineDesc.bindingLayouts);
    m_csComputeAvgLayerRadiance.init(m_device, *shaderFactory, "caustica/shaders/render/processingPasses/DenoisingGuidesPass.hlsl", "computeAvgLayerRadiance", std::vector<caustica::ShaderMacro>(), pipelineDesc.bindingLayouts);
    m_csDebugViz.init(m_device, *shaderFactory, "caustica/shaders/render/processingPasses/DenoisingGuidesPass.hlsl", "DebugViz", std::vector<caustica::ShaderMacro>(), pipelineDesc.bindingLayouts);

    //m_constantBuffer = m_device->createBuffer(caustica::rhi::utils::CreateVolatileConstantBufferDesc(sizeof(DenoisingGuidesPassConstants), "DenoisingGuidesPassConstants", caustica::c_MaxRenderPassConstantBufferVersions));
}

DenoisingGuidesPass::~DenoisingGuidesPass( )
{
}

#pragma optimize("", off)

void DenoisingGuidesPass::denoiseSpecHitT(caustica::rhi::CommandList* commandList, caustica::rhi::BindingSetHandle bindingSet)
{
    RAII_SCOPE(commandList->beginMarker("denoiseSpecHitT"); , commandList->endMarker(); );

    int threadGroupCountX = div_ceil(m_renderTargets->renderSize.x, DGB_2D_THREADGROUP_SIZE);
    int threadGroupCountY = div_ceil(m_renderTargets->renderSize.y, DGB_2D_THREADGROUP_SIZE);

    static int passCount = 1;
    DenoisingGuidesPassConstants consts;
    for( int pass = 0; pass < passCount; pass++ )
    {
        // ping
        commandList->setTextureState(m_renderTargets->specularHitT, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
        consts = DenoisingGuidesPassConstants { .RenderResolution = m_renderTargets->renderSize, .DisplayResolution = m_renderTargets->displaySize, .DebugView = (int)0, .Ping = 1 };
        m_csDenoiseSpecHitT.execute(commandList, threadGroupCountX, threadGroupCountY, 1, bindingSet, nullptr, nullptr, &consts, sizeof(consts));
        // pong
        commandList->setTextureState(m_renderTargets->specularHitT, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_renderTargets->scratchFloat1, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
        consts = DenoisingGuidesPassConstants { .RenderResolution = m_renderTargets->renderSize, .DisplayResolution = m_renderTargets->displaySize, .DebugView = (int)0, .Ping = 0 };
        m_csDenoiseSpecHitT.execute(commandList, threadGroupCountX, threadGroupCountY, 1, bindingSet, nullptr, nullptr, &consts, sizeof(consts));
    }
    commandList->setTextureState(m_renderTargets->specularHitT, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
}

void DenoisingGuidesPass::computeAvgLayerRadiance(caustica::rhi::CommandList* commandList, caustica::rhi::BindingSetHandle bindingSet)
{
    RAII_SCOPE(commandList->beginMarker("computeAvgLayerRadiance"); , commandList->endMarker(); );

    const auto& texDesc = m_renderTargets->denoiserAvgLayerRadianceHalfRes->getDesc();
    int halfWidth = (int)texDesc.width;
    int halfHeight = (int)texDesc.height;

    int threadGroupCountX = div_ceil(halfWidth, DGB_2D_THREADGROUP_SIZE);
    int threadGroupCountY = div_ceil(halfHeight, DGB_2D_THREADGROUP_SIZE);

    DenoisingGuidesPassConstants consts { .RenderResolution = m_renderTargets->renderSize, .DisplayResolution = m_renderTargets->displaySize, .DebugView = 0, .Ping = 0 };
    m_csComputeAvgLayerRadiance.execute(commandList, threadGroupCountX, threadGroupCountY, 1, bindingSet, nullptr, nullptr, &consts, sizeof(consts));

    commandList->setTextureState(m_renderTargets->denoiserAvgLayerRadianceHalfRes, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
}

void DenoisingGuidesPass::renderDebugViz( caustica::rhi::CommandList * commandList, DebugViewType debugView, caustica::rhi::BindingSetHandle bindingSet )
{
    //if( !m_settings.enabled )
    //    return;

    
    DenoisingGuidesPassConstants consts { .RenderResolution = m_renderTargets->renderSize, .DisplayResolution = m_renderTargets->displaySize, .DebugView = (int)debugView } ;

    RAII_SCOPE(commandList->beginMarker("DebugViz");, commandList->endMarker(); );

    int threadGroupCountX = div_ceil(consts.RenderResolution.x, DGB_2D_THREADGROUP_SIZE);
    int threadGroupCountY = div_ceil(consts.RenderResolution.y, DGB_2D_THREADGROUP_SIZE);

    m_csDebugViz.execute(commandList, threadGroupCountX, threadGroupCountY, 1, bindingSet, nullptr, nullptr, &consts, sizeof(consts) );
}

bool DenoisingGuidesPass::debugGUI(float indent)
{
    //ImGui::PushItemWidth(120.0f);

    // ImGui::InputInt2("BoxPos", &m_settings.BoxPos.x);
    // ImGui::InputInt2("BoxSize", &m_settings.BoxSize.x);

    //ImGui::PopItemWidth();
    return false;
}

