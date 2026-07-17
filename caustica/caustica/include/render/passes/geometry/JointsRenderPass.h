#pragma once

#include <scene/View.h>
#include <rhi/nvrhi.h>

#include <memory>
#include <vector>

namespace caustica
{
    class ShaderFactory;

    namespace scene
    {
    class SceneRenderData;
    }
}

namespace caustica::render
{
    // A rasterization pass that draws lines for each joint
    // of all the animated skeletons in a scene (debugging feature)

    class JointsRenderPass
    {
    protected:
        nvrhi::DeviceHandle m_device;

        nvrhi::BufferHandle m_VertexBuffer;
        nvrhi::BufferHandle m_ConstantsBuffer;

        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_PixelShader;

        nvrhi::InputLayoutHandle m_InputLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::GraphicsPipelineHandle m_Pipeline;

        nvrhi::BufferHandle createVertexBuffer(uint32_t numVertices) const;

    protected:

        struct Vertex {
            dm::float3 position;
            uint32_t color;
        };

        std::vector<Vertex> m_Vertices;

        void updateVertices(const caustica::scene::SceneRenderData& renderData);

    public:

        JointsRenderPass(nvrhi::IDevice* device);

        void init(caustica::ShaderFactory& shaderFactory);

        void resetCaches();

        void renderView(
            nvrhi::ICommandList* commandList,
            const caustica::IView* view,
            nvrhi::IFramebuffer* framebuffer,
            const caustica::scene::SceneRenderData& renderData);
    };
} // end namespace caustica::render
