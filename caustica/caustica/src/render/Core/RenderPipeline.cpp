#include "render/Core/RenderPipeline.h"
#include "render/Core/RenderTargets.h"
#include "render/Core/CommonRenderPasses.h"
#include "render/Core/BindingCache.h"
#include "render/Core/IRenderPass.h"
#include "assets/cache/TextureCache.h"

#include <cassert>

RenderPipeline::RenderPipeline(nvrhi::IDevice* device,
                               std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
{
    assert(m_device);

    m_commandList = m_device->createCommandList();

    commonPasses = std::make_shared<caustica::CommonRenderPasses>(m_device, m_shaderFactory);
    bindingCache = std::make_shared<caustica::BindingCache>(m_device);
}

RenderPipeline::~RenderPipeline()
{
    DestroyRenderTargets();

    // Release binding resources before the device goes away
    bindingSet     = nullptr;
    bindingLayout  = nullptr;
    bindlessLayout = nullptr;
    constantBuffer = nullptr;
    m_commandList  = nullptr;
}

// ---------------------------------------------------------------------------
// Command list
// ---------------------------------------------------------------------------

void RenderPipeline::OpenCommandList()
{
    m_commandList->open();
}

void RenderPipeline::CloseAndSubmitCommandList()
{
    m_commandList->close();
    m_device->executeCommandList(m_commandList);
}

// ---------------------------------------------------------------------------
// Render target lifecycle
// ---------------------------------------------------------------------------

bool RenderPipeline::IsRenderTargetUpdateRequired(dm::uint2 renderSize,
                                                  dm::uint2 displaySize,
                                                  dm::uint  sampleCount) const
{
    if (renderTargets == nullptr)
        return true;
    return renderTargets->IsUpdateRequired(renderSize, displaySize, sampleCount);
}

void RenderPipeline::CreateRenderTargets(dm::uint2 renderSize,
                                         dm::uint2 displaySize,
                                         bool      enableMotionVectors,
                                         bool      useReverseProjection,
                                         int       backbufferCount)
{
    // Destroy existing targets first
    DestroyRenderTargets();

    // Clear binding cache so stale descriptors referencing old textures are purged
    if (bindingCache)
        bindingCache->Clear();

    renderTargets = std::make_unique<RenderTargets>();
    renderTargets->Init(m_device, renderSize, displaySize,
                        enableMotionVectors, useReverseProjection, backbufferCount);

    m_renderSize         = renderSize;
    m_displaySize        = displaySize;
    m_lastRTWidth        = renderSize.x;
    m_lastRTHeight       = renderSize.y;
    m_hasRenderTargets   = true;

    // Notify registered passes of the resize
    for (auto& entry : m_passes)
    {
        if (entry.pass)
            entry.pass->BackBufferResized(renderSize.x, renderSize.y, 1);
    }
}

void RenderPipeline::DestroyRenderTargets()
{
    if (renderTargets)
    {
        // Notify passes before destroying targets
        for (auto& entry : m_passes)
        {
            if (entry.pass)
                entry.pass->BackBufferResized(0, 0, 1);
        }

        renderTargets.reset();
    }
    m_hasRenderTargets = false;
}

// ---------------------------------------------------------------------------
// Binding set
// ---------------------------------------------------------------------------

void RenderPipeline::RecreateBindingSet(const nvrhi::BindingSetDesc& desc,
                                        nvrhi::IBindingLayout*        layout)
{
    bindingSet = m_device->createBindingSet(desc, layout);
}

// ---------------------------------------------------------------------------
// Pass registration
// ---------------------------------------------------------------------------

void RenderPipeline::RegisterPass(const std::string& name, caustica::IRenderPass* pass)
{
    m_passes.push_back({name, pass, nullptr});

    // If we already have render targets, notify the new pass of current size
    if (m_hasRenderTargets && pass)
        pass->BackBufferResized(m_lastRTWidth, m_lastRTHeight, 1);
}

void RenderPipeline::UnregisterPass(const std::string& name)
{
    for (auto it = m_passes.begin(); it != m_passes.end(); ++it)
    {
        if (it->name == name)
        {
            m_passes.erase(it);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void RenderPipeline::BeginFrame()
{
    m_commandList->open();
}

void RenderPipeline::ExecuteAll(nvrhi::IFramebuffer* framebuffer)
{
    for (auto& entry : m_passes)
    {
        if (entry.pass)
            entry.pass->Render(framebuffer);
    }
}

void RenderPipeline::EndFrame()
{
    m_commandList->close();
    m_device->executeCommandList(m_commandList);
    AdvanceFrame();
}

void RenderPipeline::AdvanceFrame()
{
    m_frameIndex++;
}
