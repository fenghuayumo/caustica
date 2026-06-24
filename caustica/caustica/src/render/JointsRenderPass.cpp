#include <render/JointsRenderPass.h>
#include <scene/Scene.h>
#include <assets/loader/ShaderFactory.h>
#include <math/math.h>
#include <rhi/utils.h>
#include <array>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/joints_main_ps.dxbc.h"
#include "compiled_shaders/passes/joints_main_vs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/joints_main_ps.dxil.h"
#include "compiled_shaders/passes/joints_main_vs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/joints_main_ps.spirv.h"
#include "compiled_shaders/passes/joints_main_vs.spirv.h"
#endif
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

#include <shaders/view_cb.h>

namespace caustica::render
{

    caustica::render::JointsRenderPass::JointsRenderPass(nvrhi::IDevice* device) : m_Device(device) { }

    void caustica::render::JointsRenderPass::Init(caustica::ShaderFactory& shaderFactory)
    {
        m_ConstantsBuffer = m_Device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(PlanarViewConstants), "JointsWidget_Constants", 16));

        std::vector<ShaderMacro> macros;

        m_VertexShader = shaderFactory.CreateAutoShader("engine/passes/joints.hlsl", "main_vs", CAUSTICA_MAKE_PLATFORM_SHADER(g_joints_main_vs), &macros, nvrhi::ShaderType::Vertex);

        m_PixelShader = shaderFactory.CreateAutoShader("engine/passes/joints.hlsl", "main_ps", CAUSTICA_MAKE_PLATFORM_SHADER(g_joints_main_ps), &macros, nvrhi::ShaderType::Pixel);
     
        nvrhi::BindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        };
        m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

        nvrhi::VertexAttributeDesc inputDescs[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RGB32_FLOAT)
                .setOffset(offsetof(Vertex, position))
                .setElementStride(sizeof(Vertex)),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::R32_UINT)
                .setOffset(offsetof(Vertex, color))
                .setElementStride(sizeof(Vertex)),
        };
        m_InputLayout = m_Device->createInputLayout(inputDescs, uint32_t(std::size(inputDescs)), m_VertexShader);
    }

    nvrhi::BufferHandle caustica::render::JointsRenderPass::CreateVertexBuffer(uint32_t numVertices) const
    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.isVertexBuffer = true;
        bufferDesc.byteSize = numVertices * sizeof(Vertex);
        bufferDesc.debugName = "JointWidget_VertexBuffer";
        bufferDesc.canHaveTypedViews = true;
        bufferDesc.canHaveRawViews = true;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveRawViews = true;
        return m_Device->createBuffer(bufferDesc);
    }

    void JointsRenderPass::ResetCaches()
    {
        m_Vertices.clear();
    }

    void caustica::render::JointsRenderPass::UpdateVertices(const caustica::SceneGraph& sceneGraph)
    {
        static const uint32_t blue = vectorToSnorm8(float3(0.f, 0.f, 1.f));
        static const uint32_t red = vectorToSnorm8(float3(1.f, 0.f, 0.f));

        const auto& skinnedInstances = sceneGraph.GetSkinnedMeshInstances();

        uint32_t vertexId = 0;
        for (const auto& skinnedInstance : skinnedInstances)
        {
            const affine3& localToWorldTransform = skinnedInstance->GetNode()->GetLocalToWorldTransformFloat();

            daffine3 worldToRoot = inverse(daffine3(localToWorldTransform));

            uint32_t njoints = (uint32_t)skinnedInstance->joints.size();

            Vertex a = { float3(), blue };
            Vertex b = { float3(), blue };

            for (size_t i = 0; i < njoints; i++)
            {
                const SkinnedMeshJoint& joint = skinnedInstance->joints[i];

                auto jointNode = joint.node.lock();

                dm::float4x4 jointMatrix = dm::affineToHomogeneous(dm::affine3(jointNode->GetLocalToWorldTransform() * worldToRoot));

                a.position = (float4(0.f, 0.f, 0.f, 1.f) * jointMatrix).xyz();

                if (SceneGraphNode* parentNode = jointNode->GetParent())
                {
                    dm::float4x4 parentMatrix = dm::affineToHomogeneous(dm::affine3(parentNode->GetLocalToWorldTransform() * worldToRoot));

                    b.position = (float4(0.f, 0.f, 0.f, 1.f) * parentMatrix).xyz();
                }
                else
                    b = Vertex({ float3(0.f, 0.f, 0.f), red });

                float4x4 instanceTransform = affineToHomogeneous(localToWorldTransform);

                a.position = (float4(a.position, 1.f) * instanceTransform).xyz();
                b.position = (float4(b.position, 1.f) * instanceTransform).xyz();

                m_Vertices[vertexId++] = a;
                m_Vertices[vertexId++] = b;               
            }
        }
    }

    void JointsRenderPass::RenderView(
        nvrhi::ICommandList* commandList, 
        const caustica::IView* view,
        nvrhi::IFramebuffer* framebuffer,
        std::shared_ptr<caustica::SceneGraph const> sceneGraph)
    {
      
        const auto& skinnedInstances = sceneGraph->GetSkinnedMeshInstances();

        if (m_Vertices.empty())
        {
            size_t totalJoints = 0;
            for (const auto& skinnedInstance : skinnedInstances)
                totalJoints += skinnedInstance->joints.size();
            
            size_t numVertices = totalJoints * 2;
            
            m_Vertices.resize(numVertices);
            
            if (!m_Vertices.empty())
                m_VertexBuffer = CreateVertexBuffer((uint32_t)numVertices);
            else
                return;
        }
        
        commandList->beginMarker("JointsRenderPass");

        PlanarViewConstants constants;
        view->FillPlanarViewConstants(constants);   
        commandList->writeBuffer(m_ConstantsBuffer, &constants, sizeof(PlanarViewConstants));

        UpdateVertices(*sceneGraph);

        commandList->writeBuffer(m_VertexBuffer, m_Vertices.data(), m_Vertices.size() * sizeof(Vertex));
    
        nvrhi::FramebufferInfo const& framebufferInfo = framebuffer->getFramebufferInfo();

        if (!m_Pipeline)
        {
            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.primType = nvrhi::PrimitiveType::LineList;
            pipelineDesc.inputLayout = m_InputLayout;
            pipelineDesc.bindingLayouts = { m_BindingLayout };
            pipelineDesc.VS = m_VertexShader;
            pipelineDesc.PS = m_PixelShader;

            pipelineDesc.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
            pipelineDesc.renderState.rasterState.setFrontCounterClockwise(view->IsMirrored());
            pipelineDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
            pipelineDesc.renderState.depthStencilState.disableDepthTest();

            m_Pipeline = m_Device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
        }

        assert(m_Pipeline->getFramebufferInfo() == framebufferInfo);

        if (!m_BindingSet)
        {
            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantsBuffer),
            };
            m_BindingSet = m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
        }

        nvrhi::GraphicsState state;
        state.vertexBuffers = { { m_VertexBuffer, 0, 0 } };
        state.pipeline = m_Pipeline;
        state.framebuffer = framebuffer;
        state.viewport = view->GetViewportState();
        state.bindings = { m_BindingSet };

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = (uint32_t)m_Vertices.size();

        commandList->draw(args);

        commandList->endMarker();
    }

} // end namespace caustica::render
