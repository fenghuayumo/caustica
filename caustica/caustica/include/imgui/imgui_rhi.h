#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <stdint.h>
#include <atomic>

#include <rhi/rhi.h>

#include <imgui.h>

namespace caustica
{
    class ShaderFactory;
}

namespace caustica
{
    // ImGui backend for Caustica RHI.
    struct ImGui_RHI
    {
        caustica::rhi::DeviceHandle m_device;
        caustica::rhi::CommandListHandle m_commandList;

        caustica::rhi::ShaderHandle vertexShader;
        caustica::rhi::ShaderHandle pixelShader;
        caustica::rhi::InputLayoutHandle shaderAttribLayout;

        caustica::rhi::TextureHandle fontTexture;
        caustica::rhi::SamplerHandle fontSampler;

        caustica::rhi::BufferHandle vertexBuffer;
        caustica::rhi::BufferHandle indexBuffer;

        caustica::rhi::BindingLayoutHandle bindingLayout;
        caustica::rhi::GraphicsPipelineDesc basePSODesc;

        caustica::rhi::GraphicsPipelineHandle pso;
        std::unordered_map<caustica::rhi::Texture*, caustica::rhi::BindingSetHandle> bindingsCache;

        std::vector<ImDrawVert> vtxBuffer;
        std::vector<ImDrawIdx> idxBuffer;

        bool init(caustica::rhi::Device* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
        bool updateFontTexture();
        // Snapshot ImGui::GetDrawData() for safe consumption on the render thread.
        void captureDrawData();
        bool render(caustica::rhi::Framebuffer* framebuffer);
        void backbufferResizing();

    private:
        struct CapturedDrawCmd
        {
            ImVec4 clipRect{};
            caustica::rhi::Texture* texture = nullptr;
            uint32_t elemCount = 0;
            uint32_t idxOffset = 0;
            uint32_t vtxOffset = 0;
        };

        struct CapturedFrame
        {
            std::vector<ImDrawVert> vtx;
            std::vector<ImDrawIdx> idx;
            std::vector<CapturedDrawCmd> cmds;
            ImVec2 displaySize = ImVec2(0.f, 0.f);
            ImVec2 framebufferScale = ImVec2(1.f, 1.f);
            bool valid = false;
        };

        bool reallocateBuffer(caustica::rhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer);

        caustica::rhi::GraphicsPipeline* getPSO(caustica::rhi::FramebufferInfo const& framebufferInfo);
        caustica::rhi::BindingSet* getBindingSet(caustica::rhi::Texture* texture);
        bool updateGeometry(caustica::rhi::CommandList* commandList, const CapturedFrame& frame);

        CapturedFrame m_frames[2];
        std::atomic<int> m_readSlot{ -1 };
        int m_writeSlot = 0;
    };
}
