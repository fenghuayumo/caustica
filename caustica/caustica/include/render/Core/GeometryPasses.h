#pragma once

#include <scene/View.h>
#include <rhi/nvrhi.h>

namespace caustica
{
    class SceneGraphNode;
    struct MeshInfo;
    struct MeshGeometry;
    class MeshInstance;
    struct Material;
    struct BufferGroup;
    class FramebufferFactory;
}

namespace caustica::render
{
    class IDrawStrategy;

    struct DrawItem
    {
        const caustica::MeshInstance* instance;
        const caustica::MeshInfo* mesh;
        const caustica::MeshGeometry* geometry;
        const caustica::Material* material;
        const caustica::BufferGroup* buffers;
        float distanceToCamera;
        nvrhi::RasterCullMode cullMode;
    };

    class GeometryPassContext
    {
    };
    
    class IGeometryPass
    {
    public:
        [[nodiscard]] virtual caustica::ViewType::Enum GetSupportedViewTypes() const = 0;
        virtual void SetupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev) = 0;
        virtual bool SetupMaterial(GeometryPassContext& context, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) = 0;
        virtual void SetupInputBuffers(GeometryPassContext& context, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state) = 0;
        virtual void SetPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) = 0;
        virtual ~IGeometryPass() = default;
    };

    void RenderView(
        nvrhi::ICommandList* commandList, 
        const caustica::IView* view, 
        const caustica::IView* viewPrev, 
        nvrhi::IFramebuffer* framebuffer,
        IDrawStrategy& drawStrategy,
        IGeometryPass& pass,
        GeometryPassContext& passContext,
        bool materialEvents = false);

    void RenderCompositeView(
        nvrhi::ICommandList* commandList,
        const caustica::ICompositeView* compositeView,
        const caustica::ICompositeView* compositeViewPrev,
        caustica::FramebufferFactory& framebufferFactory,
        const std::shared_ptr<caustica::SceneGraphNode>& rootNode,
        IDrawStrategy& drawStrategy,
        IGeometryPass& pass,
        GeometryPassContext& passContext,
        const char* passEvent = nullptr,
        bool materialEvents = false);
}