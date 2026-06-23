/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <engine/View.h>
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