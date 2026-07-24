#include <stddef.h>

#include <imgui.h>

#include <rhi/rhi.h>
#include <assets/loader/ShaderFactory.h>
#include <imgui/imgui_rhi.h>
#include <core/log.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/imgui_vertex.dxbc.h"
#include "compiled_shaders/imgui_pixel.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/imgui_vertex.dxil.h"
#include "compiled_shaders/imgui_pixel.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/imgui_vertex.spirv.h"
#include "compiled_shaders/imgui_pixel.spirv.h"
#endif
#endif

using namespace caustica;
using namespace caustica;

struct VERTEX_CONSTANT_BUFFER
{
    float        mvp[4][4];
};

bool ImGui_RHI::updateFontTexture()
{
    ImGuiIO& io = ImGui::GetIO();

    // If the font texture exists and is bound to ImGui, we're done.
    // Note: ImGui_Renderer will reset io.Fonts->TexRef when new fonts are added.
    if (fontTexture && io.Fonts->TexRef.GetTexID())
        return true;

    unsigned char *pixels;
    int width, height;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels)
        return false;

    caustica::rhi::TextureDesc textureDesc;
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.format = caustica::rhi::Format::RGBA8_UNORM;
    textureDesc.debugName = "ImGui font texture";

    fontTexture = m_device->createTexture(textureDesc);

    if (fontTexture == nullptr)
        return false;

    m_commandList->open();

    m_commandList->beginTrackingTextureState(fontTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);

    m_commandList->writeTexture(fontTexture, 0, 0, pixels, width * 4);

    m_commandList->setPermanentTextureState(fontTexture, caustica::rhi::ResourceStates::ShaderResource);
    m_commandList->commitBarriers();

    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    io.Fonts->TexRef = ImTextureRef(fontTexture.Get());

    return true;
}

bool ImGui_RHI::init(caustica::rhi::Device* device, std::shared_ptr<ShaderFactory> shaderFactory)
{
    m_device = device;

    m_commandList = m_device->createCommandList();

    vertexShader = shaderFactory->createAutoShader("engine/imgui_vertex", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_imgui_vertex), nullptr, caustica::rhi::ShaderType::Vertex);
    pixelShader = shaderFactory->createAutoShader("engine/imgui_pixel", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_imgui_pixel), nullptr, caustica::rhi::ShaderType::Pixel);
    
    if (!vertexShader || !pixelShader)
    {
        caustica::error("Failed to create an ImGUI shader");
        return false;
    } 

    // create attribute layout object
    caustica::rhi::VertexAttributeDesc vertexAttribLayout[] = {
        { "POSITION", caustica::rhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
        { "TEXCOORD", caustica::rhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
        { "COLOR",    caustica::rhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
    };

    shaderAttribLayout = m_device->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), vertexShader);

    // create PSO
    {
        caustica::rhi::BlendState blendState;
        blendState.targets[0].setBlendEnable(true)
            .setSrcBlend(caustica::rhi::BlendFactor::SrcAlpha)
            .setDestBlend(caustica::rhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(caustica::rhi::BlendFactor::InvSrcAlpha)
            .setDestBlendAlpha(caustica::rhi::BlendFactor::Zero);

        auto rasterState = caustica::rhi::RasterState()
            .setFillSolid()
            .setCullNone()
            .setScissorEnable(true)
            .setDepthClipEnable(true);

        auto depthStencilState = caustica::rhi::DepthStencilState()
            .disableDepthTest()
            .enableDepthWrite()
            .disableStencil()
            .setDepthFunc(caustica::rhi::ComparisonFunc::Always);

        caustica::rhi::RenderState renderState;
        renderState.blendState = blendState;
        renderState.depthStencilState = depthStencilState;
        renderState.rasterState = rasterState;

        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::All;
        layoutDesc.bindings = { 
            caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 2),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Sampler(0) 
        };
        bindingLayout = m_device->createBindingLayout(layoutDesc);

        basePSODesc.primType = caustica::rhi::PrimitiveType::TriangleList;
        basePSODesc.inputLayout = shaderAttribLayout;
        basePSODesc.VS = vertexShader;
        basePSODesc.PS = pixelShader;
        basePSODesc.renderState = renderState;
        basePSODesc.bindingLayouts = { bindingLayout };
    }

    {
        const auto desc = caustica::rhi::SamplerDesc()
            .setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap)
            .setAllFilters(true);

        fontSampler = m_device->createSampler(desc);

        if (fontSampler == nullptr)
            return false;
    }

    return true;
}

bool ImGui_RHI::reallocateBuffer(caustica::rhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, const bool indexBuffer)
{
    if (buffer == nullptr || size_t(buffer->getDesc().byteSize) < requiredSize)
    {
        caustica::rhi::BufferDesc desc;
        desc.byteSize = uint32_t(reallocateSize);
        desc.structStride = 0;
        desc.debugName = indexBuffer ? "ImGui index buffer" : "ImGui vertex buffer";
        desc.canHaveUAVs = false;
        desc.isVertexBuffer = !indexBuffer;
        desc.isIndexBuffer = indexBuffer;
        desc.isDrawIndirectArgs = false;
        desc.isVolatile = false;
        desc.initialState = indexBuffer ? caustica::rhi::ResourceStates::IndexBuffer : caustica::rhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;

        buffer = m_device->createBuffer(desc);

        if (!buffer)
        {
            return false;
        }
    }

    return true;
}

caustica::rhi::GraphicsPipeline* ImGui_RHI::getPSO(caustica::rhi::FramebufferInfo const& framebufferInfo)
{
    if (pso)
        return pso;

    pso = m_device->createGraphicsPipeline(basePSODesc, framebufferInfo);
    assert(pso);

    return pso;
}

caustica::rhi::BindingSet* ImGui_RHI::getBindingSet(caustica::rhi::Texture* texture)
{
    auto iter = bindingsCache.find(texture);
    if (iter != bindingsCache.end())
    {
        return iter->second;
    }

    caustica::rhi::BindingSetDesc desc;

    desc.bindings = {
        caustica::rhi::BindingSetItem::PushConstants(0, sizeof(float) * 2),
        caustica::rhi::BindingSetItem::Texture_SRV(0, texture),
        caustica::rhi::BindingSetItem::Sampler(0, fontSampler)
    };

    caustica::rhi::BindingSetHandle binding;
    binding = m_device->createBindingSet(desc, bindingLayout);
    assert(binding);

    bindingsCache[texture] = binding;
    return binding;
}

void ImGui_RHI::captureDrawData()
{
    ImDrawData* drawData = ImGui::GetDrawData();
    CapturedFrame& frame = m_frames[m_writeSlot];
    frame.vtx.clear();
    frame.idx.clear();
    frame.cmds.clear();
    frame.valid = false;

    if (!drawData || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0)
    {
        m_readSlot.store(-1, std::memory_order_release);
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    frame.displaySize = io.DisplaySize;
    frame.framebufferScale = io.DisplayFramebufferScale;
    if (frame.displaySize.x <= 0.f || frame.displaySize.y <= 0.f)
    {
        m_readSlot.store(-1, std::memory_order_release);
        return;
    }

    frame.vtx.resize(static_cast<size_t>(drawData->TotalVtxCount));
    frame.idx.resize(static_cast<size_t>(drawData->TotalIdxCount));

    ImDrawVert* vtxDst = frame.vtx.data();
    ImDrawIdx* idxDst = frame.idx.data();
    uint32_t vtxOffset = 0;
    uint32_t idxOffset = 0;

    const ImVec2 clipScale = frame.framebufferScale;

    for (int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

        for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
        {
            const ImDrawCmd& srcCmd = cmdList->CmdBuffer[i];
            if (!srcCmd.UserCallback)
            {
                CapturedDrawCmd cmd;
                cmd.texture = reinterpret_cast<caustica::rhi::Texture*>(srcCmd.TexRef.GetTexID());
                cmd.elemCount = srcCmd.ElemCount;
                cmd.idxOffset = idxOffset;
                cmd.vtxOffset = vtxOffset;
                cmd.clipRect = ImVec4(
                    srcCmd.ClipRect.x * clipScale.x,
                    srcCmd.ClipRect.y * clipScale.y,
                    srcCmd.ClipRect.z * clipScale.x,
                    srcCmd.ClipRect.w * clipScale.y);
                frame.cmds.push_back(cmd);
            }
            idxOffset += srcCmd.ElemCount;
        }

        vtxDst += cmdList->VtxBuffer.Size;
        idxDst += cmdList->IdxBuffer.Size;
        vtxOffset += static_cast<uint32_t>(cmdList->VtxBuffer.Size);
    }

    frame.valid = !frame.cmds.empty();
    const int published = m_writeSlot;
    m_writeSlot = 1 - m_writeSlot;
    m_readSlot.store(frame.valid ? published : -1, std::memory_order_release);
}

bool ImGui_RHI::updateGeometry(caustica::rhi::CommandList* commandList, const CapturedFrame& frame)
{
    if (!reallocateBuffer(vertexBuffer,
        frame.vtx.size() * sizeof(ImDrawVert),
        (frame.vtx.size() + 5000) * sizeof(ImDrawVert),
        false))
    {
        return false;
    }

    if (!reallocateBuffer(indexBuffer,
        frame.idx.size() * sizeof(ImDrawIdx),
        (frame.idx.size() + 5000) * sizeof(ImDrawIdx),
        true))
    {
        return false;
    }

    if (!frame.vtx.empty())
        commandList->writeBuffer(vertexBuffer, frame.vtx.data(), frame.vtx.size() * sizeof(ImDrawVert));
    if (!frame.idx.empty())
        commandList->writeBuffer(indexBuffer, frame.idx.data(), frame.idx.size() * sizeof(ImDrawIdx));

    return true;
}

bool ImGui_RHI::render(caustica::rhi::Framebuffer* framebuffer)
{
    // Swapchain FBs are cleared during backBufferResizing(); never assert-crash here.
    if (!framebuffer)
        return false;

    const int slot = m_readSlot.load(std::memory_order_acquire);
    if (slot < 0 || slot > 1)
        return false;

    const CapturedFrame& frame = m_frames[slot];
    if (!frame.valid || frame.cmds.empty())
        return false;

    m_commandList->open();
    m_commandList->beginMarker("ImGUI");

    if (!updateGeometry(m_commandList, frame))
    {
        m_commandList->close();
        return false;
    }

    float invDisplaySize[2] = { 1.f / frame.displaySize.x, 1.f / frame.displaySize.y };

    caustica::rhi::GraphicsState drawState;
    drawState.framebuffer = framebuffer;

    drawState.pipeline = getPSO(framebuffer->getFramebufferInfo());

    drawState.viewport.viewports.push_back(caustica::rhi::Viewport(
        frame.displaySize.x * frame.framebufferScale.x,
        frame.displaySize.y * frame.framebufferScale.y));
    drawState.viewport.scissorRects.resize(1);

    caustica::rhi::VertexBufferBinding vbufBinding;
    vbufBinding.buffer = vertexBuffer;
    vbufBinding.slot = 0;
    vbufBinding.offset = 0;
    drawState.vertexBuffers.push_back(vbufBinding);

    drawState.indexBuffer.buffer = indexBuffer;
    drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? caustica::rhi::Format::R16_UINT : caustica::rhi::Format::R32_UINT);
    drawState.indexBuffer.offset = 0;

    for (const CapturedDrawCmd& cmd : frame.cmds)
    {
        drawState.bindings = { getBindingSet(cmd.texture) };
        assert(drawState.bindings[0]);

        drawState.viewport.scissorRects[0] = caustica::rhi::Rect(
            int(cmd.clipRect.x),
            int(cmd.clipRect.z),
            int(cmd.clipRect.y),
            int(cmd.clipRect.w));

        caustica::rhi::DrawArguments drawArguments;
        drawArguments.vertexCount = cmd.elemCount;
        drawArguments.startIndexLocation = cmd.idxOffset;
        drawArguments.startVertexLocation = cmd.vtxOffset;

        m_commandList->setGraphicsState(drawState);
        m_commandList->setPushConstants(invDisplaySize, sizeof(invDisplaySize));
        m_commandList->drawIndexed(drawArguments);
    }

    m_commandList->endMarker();
    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return true;
}

void ImGui_RHI::backbufferResizing()
{
    pso = nullptr;
}
