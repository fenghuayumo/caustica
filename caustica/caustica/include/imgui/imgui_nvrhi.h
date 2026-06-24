#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <stdint.h>

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
        bool render(nvrhi::IFramebuffer* framebuffer);
        void backbufferResizing();

    private:
        bool reallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer);


        nvrhi::IGraphicsPipeline* getPSO(nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::IBindingSet* getBindingSet(nvrhi::ITexture* texture);
        bool updateGeometry(nvrhi::ICommandList* commandList);
    };
}