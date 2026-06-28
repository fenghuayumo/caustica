#pragma once

#include <scene/View.h>
#include <rhi/nvrhi.h>

#include <memory>
#include <vector>

namespace caustica
{
    class ShaderFactory;
    class SceneGraph;
}

namespace caustica::render
{
    // A rasterization pass that draws lines for each joint
    // of all the animated skeletons in a SceneGraph
    // (debugging feature)

    class JointsRenderPass
    {
    protected:
        nvrhi::DeviceHandle m_Device;

        nvrhi::BufferHandle m_VertexBuffer;
        nvrhi::BufferHandle m_ConstantsBuffer;

        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_PixelShader;

        nvrhi::InputLayoutHandle m_InputLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::GraphicsPipelineHandle m_Pipeline;

        nvrhi::BufferHandle CreateVertexBuffer(uint32_t numVertices) const;

    protected:

        struct Vertex {
            dm::float3 position;
            uint32_t color;
        };

        std::vector<Vertex> m_Vertices;

        void UpdateVertices(const caustica::SceneGraph& sceneGraph);

    public:

        JointsRenderPass(nvrhi::IDevice* device);

        void Init(caustica::ShaderFactory& shaderFactory);

        void ResetCaches();

        void RenderView(
            nvrhi::ICommandList* commandList,
            const caustica::IView* view,
            nvrhi::IFramebuffer* framebuffer,
            std::shared_ptr<caustica::SceneGraph const> sceneGraph);
    };
} // end namespace caustica::render