#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <stdint.h>
#include <atomic>

#include <rhi/nvrhi.h>

#include <imgui.h>

namespace caustica
{
    class ShaderFactory;
}

namespace caustica
{
    struct ImGui_NVRHI
    {
        nvrhi::DeviceHandle m_device;
        nvrhi::CommandListHandle m_commandList;

        nvrhi::ShaderHandle vertexShader;
        nvrhi::ShaderHandle pixelShader;
        nvrhi::InputLayoutHandle shaderAttribLayout;

        nvrhi::TextureHandle fontTexture;
        nvrhi::SamplerHandle fontSampler;

        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;

        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::GraphicsPipelineDesc basePSODesc;

        nvrhi::GraphicsPipelineHandle pso;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> bindingsCache;

        std::vector<ImDrawVert> vtxBuffer;
        std::vector<ImDrawIdx> idxBuffer;

        bool init(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
        bool updateFontTexture();
        // Snapshot ImGui::GetDrawData() for safe consumption on the render thread.
        void captureDrawData();
        bool render(nvrhi::IFramebuffer* framebuffer);
        void backbufferResizing();

    private:
        struct CapturedDrawCmd
        {
            ImVec4 clipRect{};
            nvrhi::ITexture* texture = nullptr;
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

        bool reallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer);

        nvrhi::IGraphicsPipeline* getPSO(nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::IBindingSet* getBindingSet(nvrhi::ITexture* texture);
        bool updateGeometry(nvrhi::ICommandList* commandList, const CapturedFrame& frame);

        CapturedFrame m_frames[2];
        std::atomic<int> m_readSlot{ -1 };
        int m_writeSlot = 0;
    };
}
