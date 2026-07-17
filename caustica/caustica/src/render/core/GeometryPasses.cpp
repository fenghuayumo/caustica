#include <render/core/GeometryPasses.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneDrawList.h>
#include <render/core/FramebufferFactory.h>
#include <backend/ViewRhiConversion.h>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

void caustica::render::renderView(
    nvrhi::ICommandList* commandList, 
    const IView* view, 
    const IView* viewPrev,
    nvrhi::IFramebuffer* framebuffer,
    std::span<const DrawCommand> drawCommands,
    IGeometryPass& pass,
    GeometryPassContext& passContext,
    bool materialEvents)
{
    pass.setupView(passContext, commandList, view, viewPrev);

    const Material* lastMaterial = nullptr;
    const BufferGroup* lastBuffers = nullptr;
    nvrhi::RasterCullMode lastCullMode = nvrhi::RasterCullMode::Back;

    bool drawMaterial = true;
    bool stateValid = false;

    const Material* eventMaterial = nullptr;

    nvrhi::GraphicsState graphicsState;
    graphicsState.framebuffer = framebuffer;
    graphicsState.viewport = toNvrhi(view->getViewportState());

    nvrhi::DrawArguments currentDraw;
    currentDraw.instanceCount = 0;

    auto flushDraw = [commandList, materialEvents, &graphicsState, &currentDraw, &eventMaterial, &pass, &passContext](const Material* material)
    {
        if (currentDraw.instanceCount == 0)
            return;

        if (materialEvents && material != eventMaterial)
        {
            if (eventMaterial)
                commandList->endMarker();

            if (material->name.empty())
            {
                eventMaterial = nullptr;
            }
            else
            {
                commandList->beginMarker(material->name.c_str());
                eventMaterial = material;
            }
        }

        pass.setPushConstants(passContext, commandList, graphicsState, currentDraw);

        commandList->drawIndexed(currentDraw);
        currentDraw.instanceCount = 0;
    };
    
    for (const DrawCommand& item : drawCommands)
    {
        if (item.material == nullptr)
            continue;

        bool newBuffers = item.buffers != lastBuffers;
        bool newMaterial = item.material != lastMaterial || item.cullMode != lastCullMode;

        if (newBuffers || newMaterial)
            flushDraw(lastMaterial);

        if (newBuffers)
        {
            pass.setupInputBuffers(passContext, item.buffers, graphicsState);
            lastBuffers = item.buffers;
            stateValid = false;
        }

        if (newMaterial)
        {
            drawMaterial = pass.setupMaterial(passContext, item.material, item.cullMode, graphicsState);
            lastMaterial = item.material;
            lastCullMode = item.cullMode;
            stateValid = false;
        }

        if (drawMaterial)
        {
            if (!stateValid)
            {
                commandList->setGraphicsState(graphicsState);
                stateValid = true;
            }

            nvrhi::DrawArguments args;
            args.vertexCount = item.geometry->numIndices;
            args.instanceCount = 1;
            args.startVertexLocation = item.mesh->vertexOffset + item.geometry->vertexOffsetInMesh;
            args.startIndexLocation = item.mesh->indexOffset + item.geometry->indexOffsetInMesh;
            args.startInstanceLocation = item.instanceIndex;

            if (currentDraw.instanceCount > 0 && 
                currentDraw.startIndexLocation == args.startIndexLocation && 
                currentDraw.startInstanceLocation + currentDraw.instanceCount == args.startInstanceLocation)
            {
                currentDraw.instanceCount += 1;
            }
            else
            {
                flushDraw(item.material);
                currentDraw = args;
            }
        }
    }

    flushDraw(lastMaterial);

    if (materialEvents && eventMaterial)
        commandList->endMarker();
}

void caustica::render::renderCompositeView(
    nvrhi::ICommandList* commandList,
    const ICompositeView* compositeView,
    const ICompositeView* compositeViewPrev,
    FramebufferFactory& framebufferFactory,
    const caustica::scene::SceneRenderData& renderData,
    MeshDrawDomain domain,
    IGeometryPass& pass,
    GeometryPassContext& passContext,
    const DrawListBuildOptions& drawOptions,
    const char* passEvent,
    bool materialEvents)
{
    if (passEvent)
        commandList->beginMarker(passEvent);

    ViewType::Enum supportedViewTypes = pass.getSupportedViewTypes();

    if (compositeViewPrev)
    {
        assert(compositeView->getNumChildViews(supportedViewTypes) == compositeViewPrev->getNumChildViews(supportedViewTypes));
    }

    std::vector<DrawCommand> drawCommands;

    for (uint viewIndex = 0; viewIndex < compositeView->getNumChildViews(supportedViewTypes); viewIndex++)
    {
        const IView* view = compositeView->getChildView(supportedViewTypes, viewIndex);
        const IView* viewPrev = compositeViewPrev ? compositeViewPrev->getChildView(supportedViewTypes, viewIndex) : nullptr;

        assert(view != nullptr);

        if (domain == MeshDrawDomain::Opaque)
            scene::buildOpaqueDrawList(renderData, *view, drawCommands);
        else
            scene::buildTransparentDrawList(renderData, *view, drawCommands, drawOptions);

        nvrhi::IFramebuffer* framebuffer = framebufferFactory.getFramebuffer(*view);

        renderView(commandList, view, viewPrev, framebuffer, drawCommands, pass, passContext, materialEvents);
    }

    if (passEvent)
        commandList->endMarker();
}
