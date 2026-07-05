#pragma once

#include <scene/SceneDrawList.h>
#include <scene/View.h>
#include <rhi/nvrhi.h>
#include <span>

namespace caustica
{
    class Scene;
    struct MeshInfo;
    struct MeshGeometry;
    struct Material;
    struct BufferGroup;
    class FramebufferFactory;
}

namespace caustica::render
{
    using DrawCommand = scene::DrawCommand;
    using DrawItem = DrawCommand;
    using MeshDrawDomain = scene::MeshDrawDomain;
    using DrawListBuildOptions = scene::DrawListBuildOptions;

    class GeometryPassContext
    {
    };
    
    class IGeometryPass
    {
    public:
        [[nodiscard]] virtual caustica::ViewType::Enum getSupportedViewTypes() const = 0;
        virtual void setupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev) = 0;
        virtual bool setupMaterial(GeometryPassContext& context, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) = 0;
        virtual void setupInputBuffers(GeometryPassContext& context, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state) = 0;
        virtual void setPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) = 0;
        virtual ~IGeometryPass() = default;
    };

    void renderView(
        nvrhi::ICommandList* commandList, 
        const caustica::IView* view, 
        const caustica::IView* viewPrev, 
        nvrhi::IFramebuffer* framebuffer,
        std::span<const DrawCommand> drawCommands,
        IGeometryPass& pass,
        GeometryPassContext& passContext,
        bool materialEvents = false);

    void renderCompositeView(
        nvrhi::ICommandList* commandList,
        const caustica::ICompositeView* compositeView,
        const caustica::ICompositeView* compositeViewPrev,
        caustica::FramebufferFactory& framebufferFactory,
        const caustica::Scene& scene,
        MeshDrawDomain domain,
        IGeometryPass& pass,
        GeometryPassContext& passContext,
        const DrawListBuildOptions& drawOptions = {},
        const char* passEvent = nullptr,
        bool materialEvents = false);
}
