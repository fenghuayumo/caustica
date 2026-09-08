#include "D3D12Backend.h"

#include <rhi/common/misc.h>
#include <iomanip>
#include <sstream>

namespace caustica::rhi::d3d12
{

    Object GraphicsPipeline::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::D3D12_RootSignature:
            return rootSignature->getNativeObject(objectType);
        case ObjectTypes::D3D12_PipelineState:
            return Object(pipelineState.Get());
        default:
            return nullptr;
        }
    }

    RefCountPtr<ID3D12PipelineState> Device::createPipelineState(const GraphicsPipelineDesc & state, RootSignature* pRS, const FramebufferInfo& fbinfo) const
    {
        if (state.renderState.singlePassStereo.enabled && !m_singlePassStereoSupported)
        {
            m_context.error("Single-pass stereo is not supported by this device");
            return nullptr;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = pRS->handle;

        Shader* shader;
        shader = checked_cast<Shader*>(state.VS.Get());
        if (shader) desc.VS = { &shader->bytecode[0], shader->bytecode.size() };

        shader = checked_cast<Shader*>(state.HS.Get());
        if (shader) desc.HS = { &shader->bytecode[0], shader->bytecode.size() };

        shader = checked_cast<Shader*>(state.DS.Get());
        if (shader) desc.DS = { &shader->bytecode[0], shader->bytecode.size() };

        shader = checked_cast<Shader*>(state.GS.Get());
        if (shader) desc.GS = { &shader->bytecode[0], shader->bytecode.size() };

        shader = checked_cast<Shader*>(state.PS.Get());
        if (shader) desc.PS = { &shader->bytecode[0], shader->bytecode.size() };


        TranslateBlendState(state.renderState.blendState, desc.BlendState);


        const DepthStencilState& depthState = state.renderState.depthStencilState;
        TranslateDepthStencilState(depthState, desc.DepthStencilState);

        if ((depthState.depthTestEnable || depthState.stencilEnable) && fbinfo.depthFormat == Format::UNKNOWN)
        {
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            m_context.messageCallback->message(MessageSeverity::Warning, "depthEnable or stencilEnable is true, but no depth target is bound");
        }

        const RasterState& rasterState = state.renderState.rasterState;
        TranslateRasterizerState(rasterState, desc.RasterizerState);

        switch (state.primType)
        {
        case PrimitiveType::PointList:
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            break;
        case PrimitiveType::LineList:
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            break;
        case PrimitiveType::TriangleList:
        case PrimitiveType::TriangleStrip:
        case PrimitiveType::TriangleFan:
        case PrimitiveType::TriangleListWithAdjacency:
        case PrimitiveType::TriangleStripWithAdjacency:
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            break;
        case PrimitiveType::PatchList:
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            break;
        default:
            m_context.error("PrimitiveType unsupported by this device");
            return nullptr;
        }

        desc.DSVFormat = getDxgiFormatMapping(fbinfo.depthFormat).rtvFormat;

        desc.SampleDesc.Count = fbinfo.sampleCount;
        desc.SampleDesc.Quality = fbinfo.sampleQuality;

        for (uint32_t i = 0; i < uint32_t(fbinfo.colorFormats.size()); i++)
        {
            desc.RTVFormats[i] = getDxgiFormatMapping(fbinfo.colorFormats[i]).rtvFormat;
        }

        InputLayout* inputLayout = checked_cast<InputLayout*>(state.inputLayout.Get());
        if (inputLayout && !inputLayout->inputElements.empty())
        {
            desc.InputLayout.NumElements = uint32_t(inputLayout->inputElements.size());
            desc.InputLayout.pInputElementDescs = &(inputLayout->inputElements[0]);
        }

        desc.NumRenderTargets = uint32_t(fbinfo.colorFormats.size());
        desc.SampleMask = ~0u;

        RefCountPtr<ID3D12PipelineState> pipelineState;

#if CAUSTICA_RHI_D3D12_WITH_NVAPI
        std::vector<const NVAPI_D3D12_PSO_EXTENSION_DESC*> extensions;

        shader = checked_cast<Shader*>(state.VS.Get()); if (shader) extensions.insert(extensions.end(), shader->extensions.begin(), shader->extensions.end());
        shader = checked_cast<Shader*>(state.HS.Get()); if (shader) extensions.insert(extensions.end(), shader->extensions.begin(), shader->extensions.end());
        shader = checked_cast<Shader*>(state.DS.Get()); if (shader) extensions.insert(extensions.end(), shader->extensions.begin(), shader->extensions.end());
        shader = checked_cast<Shader*>(state.GS.Get()); if (shader) extensions.insert(extensions.end(), shader->extensions.begin(), shader->extensions.end());
        shader = checked_cast<Shader*>(state.PS.Get()); if (shader) extensions.insert(extensions.end(), shader->extensions.begin(), shader->extensions.end());

        if (rasterState.programmableSamplePositionsEnable || rasterState.quadFillEnable)
        {
            NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC rasterizerDesc = {};
            rasterizerDesc.baseVersion = NV_PSO_EXTENSION_DESC_VER;
            rasterizerDesc.psoExtension = NV_PSO_RASTER_EXTENSION;
            rasterizerDesc.version = NV_RASTERIZER_PSO_EXTENSION_DESC_VER;

            rasterizerDesc.ProgrammableSamplePositionsEnable = rasterState.programmableSamplePositionsEnable;
            rasterizerDesc.SampleCount = rasterState.forcedSampleCount;
            memcpy(rasterizerDesc.SamplePositionsX, rasterState.samplePositionsX, sizeof(rasterState.samplePositionsX));
            memcpy(rasterizerDesc.SamplePositionsY, rasterState.samplePositionsY, sizeof(rasterState.samplePositionsY));
            rasterizerDesc.QuadFillMode = rasterState.quadFillEnable ? NVAPI_QUAD_FILLMODE_BBOX : NVAPI_QUAD_FILLMODE_DISABLED;

            extensions.push_back(&rasterizerDesc);
        }

        if (!extensions.empty())
        {
            NvAPI_Status status = NvAPI_D3D12_CreateGraphicsPipelineState(m_context.device, &desc, NvU32(extensions.size()), &extensions[0], &pipelineState);

            if (status != NVAPI_OK || pipelineState == nullptr)
            {
                m_context.error("Failed to create a graphics pipeline state object with NVAPI extensions");
                return nullptr;
            }

            return pipelineState;
        }
#endif

        const HRESULT hr = m_context.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState));

        if (FAILED(hr))
        {
            std::ostringstream ss;
            ss << "Failed to create a graphics pipeline state object, HRESULT = 0x" << std::hex << std::setw(8) << hr;
            if (auto vertexShader = checked_cast<Shader*>(state.VS.Get()))
                ss << ", VS = '" << vertexShader->desc.debugName << "'";
            if (auto pixelShader = checked_cast<Shader*>(state.PS.Get()))
                ss << ", PS = '" << pixelShader->desc.debugName << "'";
            m_context.error(ss.str());
            return nullptr;
        }

        return pipelineState;
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo)
    {
        RefCountPtr<RootSignature> pRS = getRootSignature(desc.bindingLayouts, desc.inputLayout != nullptr);

        RefCountPtr<ID3D12PipelineState> pPSO = createPipelineState(desc, pRS, fbinfo);

        return createHandleForNativeGraphicsPipeline(pRS, pPSO, desc, fbinfo);
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, rhi::Framebuffer* fb)
    {
        if (!fb)
            return nullptr;

        return createGraphicsPipeline(desc, fb->getFramebufferInfo());
    }

    caustica::rhi::GraphicsPipelineHandle Device::createHandleForNativeGraphicsPipeline(RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const GraphicsPipelineDesc& desc, const FramebufferInfo& framebufferInfo)
    {
        if (rootSignature == nullptr)
            return nullptr;

        if (pipelineState == nullptr)
            return nullptr;

        GraphicsPipeline *pso = new GraphicsPipeline();
        pso->desc = desc;
        pso->framebufferInfo = framebufferInfo;
        pso->rootSignature = rootSignature;
        pso->pipelineState = pipelineState;
        pso->requiresBlendFactor = desc.renderState.blendState.usesConstantColor(uint32_t(pso->framebufferInfo.colorFormats.size()));

        return GraphicsPipelineHandle::Create(pso);
    }

    FramebufferHandle Device::createFramebuffer(const FramebufferDesc& desc)
    {
        Framebuffer *fb = new Framebuffer(m_resources);
        fb->desc = desc;
        fb->framebufferInfo = FramebufferInfoEx(desc);

        if (!desc.colorAttachments.empty())
        {
            Texture* texture = checked_cast<Texture*>(desc.colorAttachments[0].texture);
            fb->rtWidth = texture->desc.width;
            fb->rtHeight = texture->desc.height;
        } else if (desc.depthAttachment.valid())
        {
            Texture* texture = checked_cast<Texture*>(desc.depthAttachment.texture);
            fb->rtWidth = texture->desc.width;
            fb->rtHeight = texture->desc.height;
        }

        for (size_t rt = 0; rt < desc.colorAttachments.size(); rt++)
        {
            auto& attachment = desc.colorAttachments[rt];

            Texture* texture = checked_cast<Texture*>(attachment.texture);
            assert(texture->desc.width == fb->rtWidth);
            assert(texture->desc.height == fb->rtHeight);

            DescriptorIndex index = m_resources.renderTargetViewHeap.allocateDescriptor();

            const D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = m_resources.renderTargetViewHeap.getCpuHandle(index);
            texture->createRTV(descriptorHandle.ptr, attachment.format, attachment.subresources);

            fb->RTVs.push_back(index);
            fb->textures.push_back(texture);
        }

        if (desc.depthAttachment.valid())
        {
            Texture* texture = checked_cast<Texture*>(desc.depthAttachment.texture);
            assert(texture->desc.width == fb->rtWidth);
            assert(texture->desc.height == fb->rtHeight);

            DescriptorIndex index = m_resources.depthStencilViewHeap.allocateDescriptor();

            const D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = m_resources.depthStencilViewHeap.getCpuHandle(index);
            texture->createDSV(descriptorHandle.ptr, desc.depthAttachment.subresources, desc.depthAttachment.isReadOnly);

            fb->DSV = index;
            fb->textures.push_back(texture);
        }

        return FramebufferHandle::Create(fb);
    }

    Framebuffer::~Framebuffer()
    {
        for (DescriptorIndex RTV : RTVs)
            m_resources.renderTargetViewHeap.releaseDescriptor(RTV);

        if (DSV != c_InvalidDescriptorIndex)
            m_resources.depthStencilViewHeap.releaseDescriptor(DSV);
    }

    void CommandList::bindFramebuffer(Framebuffer *fb)
    {
        static_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 16> RTVs;
        for (uint32_t rtIndex = 0; rtIndex < fb->RTVs.size(); rtIndex++)
        {
            RTVs.push_back(m_resources.renderTargetViewHeap.getCpuHandle(fb->RTVs[rtIndex]));
        }

        D3D12_CPU_DESCRIPTOR_HANDLE DSV = {};
        if (fb->desc.depthAttachment.valid())
            DSV = m_resources.depthStencilViewHeap.getCpuHandle(fb->DSV);

        m_activeCommandList->commandList->OMSetRenderTargets(UINT(RTVs.size()), RTVs.data(), false, fb->desc.depthAttachment.valid() ? &DSV : nullptr);
    }

    void CommandList::setGraphicsState(const GraphicsState& state)
    {
        GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(state.pipeline);
        Framebuffer* framebuffer = checked_cast<Framebuffer*>(state.framebuffer);

        const bool updateFramebuffer = !m_currentGraphicsStateValid || m_currentGraphicsState.framebuffer != state.framebuffer;
        const bool updateRootSignature = !m_currentGraphicsStateValid || m_currentGraphicsState.pipeline == nullptr ||
            checked_cast<GraphicsPipeline*>(m_currentGraphicsState.pipeline)->rootSignature != pso->rootSignature;

        const bool updatePipeline = !m_currentGraphicsStateValid || m_currentGraphicsState.pipeline != state.pipeline;
        const bool updateIndirectParams = !m_currentGraphicsStateValid || m_currentGraphicsState.indirectParams != state.indirectParams;
        const bool updateIndirectCountBuffer = !m_currentGraphicsStateValid || m_currentGraphicsState.indirectCountBuffer != state.indirectCountBuffer;

        const bool updateViewports = !m_currentGraphicsStateValid ||
            arraysAreDifferent(m_currentGraphicsState.viewport.viewports, state.viewport.viewports) ||
            arraysAreDifferent(m_currentGraphicsState.viewport.scissorRects, state.viewport.scissorRects);

        const bool updateBlendFactor = !m_currentGraphicsStateValid || m_currentGraphicsState.blendConstantColor != state.blendConstantColor;

        const uint8_t effectiveStencilRefValue = pso->desc.renderState.depthStencilState.dynamicStencilRef
            ? state.dynamicStencilRefValue
            : pso->desc.renderState.depthStencilState.stencilRefValue;
        const bool updateStencilRef = !m_currentGraphicsStateValid || m_currentGraphicsState.dynamicStencilRefValue != effectiveStencilRefValue;

        const bool updateIndexBuffer = !m_currentGraphicsStateValid || m_currentGraphicsState.indexBuffer != state.indexBuffer;
        const bool updateVertexBuffers = !m_currentGraphicsStateValid || arraysAreDifferent(m_currentGraphicsState.vertexBuffers, state.vertexBuffers);

        const bool updateShadingRate = !m_currentGraphicsStateValid || m_currentGraphicsState.shadingRateState != state.shadingRateState;

        uint32_t bindingUpdateMask = 0;
        if (!m_currentGraphicsStateValid || updateRootSignature)
            bindingUpdateMask = ~0u;

        if (commitDescriptorHeaps())
            bindingUpdateMask = ~0u;

        if (bindingUpdateMask == 0)
            bindingUpdateMask = arrayDifferenceMask(m_currentGraphicsState.bindings, state.bindings);

        if (updatePipeline)
        {
            bindGraphicsPipeline(pso, updateRootSignature);
            m_instance->referencedResources.push_back(pso);
        }

        if (pso->desc.renderState.depthStencilState.stencilEnable && (updatePipeline || updateStencilRef))
        {
            m_activeCommandList->commandList->OMSetStencilRef(effectiveStencilRefValue);
        }

        if (pso->requiresBlendFactor && updateBlendFactor)
        {
            m_activeCommandList->commandList->OMSetBlendFactor(&state.blendConstantColor.r);
        }

        if (updateFramebuffer)
        {
            bindFramebuffer(framebuffer);
            m_instance->referencedResources.push_back(framebuffer);
        }

        if (m_enableAutomaticBarriers && framebuffer && (m_bindingStatesDirty || updateFramebuffer))
        {
            setResourceStatesForFramebuffer(framebuffer);
        }

        setGraphicsBindings(state.bindings, bindingUpdateMask,
            checked_cast<Buffer*>(state.indirectParams), updateIndirectParams,
            checked_cast<Buffer*>(state.indirectCountBuffer), updateIndirectCountBuffer,
            pso->rootSignature);

        if (updateIndexBuffer)
        {
            D3D12_INDEX_BUFFER_VIEW IBV = {};

            if (state.indexBuffer.buffer)
            {
                Buffer* buffer = checked_cast<Buffer*>(state.indexBuffer.buffer);

                IBV.Format = getDxgiFormatMapping(state.indexBuffer.format).srvFormat;
                IBV.SizeInBytes = (UINT)(buffer->desc.byteSize - state.indexBuffer.offset);
                IBV.BufferLocation = buffer->gpuVA + state.indexBuffer.offset;

                m_instance->referencedResources.push_back(state.indexBuffer.buffer);
            }

            m_activeCommandList->commandList->IASetIndexBuffer(&IBV);
        }

        if (m_enableAutomaticBarriers && state.indexBuffer.buffer && (m_bindingStatesDirty || updateIndexBuffer))
        {
            Buffer* buffer = checked_cast<Buffer*>(state.indexBuffer.buffer);

            requireBufferState(buffer, ResourceStates::IndexBuffer);
        }

        if (updateVertexBuffers)
        {
            D3D12_VERTEX_BUFFER_VIEW VBVs[c_MaxVertexAttributes] = {};
            uint32_t maxVbIndex = 0;
            InputLayout* inputLayout = checked_cast<InputLayout*>(pso->desc.inputLayout.Get());

            for (const VertexBufferBinding& binding : state.vertexBuffers)
            {
                Buffer* buffer = checked_cast<Buffer*>(binding.buffer);

                // This is tested by the validation layer, skip invalid slots here if VL is not used.
                if (binding.slot >= c_MaxVertexAttributes)
                    continue;

                VBVs[binding.slot].StrideInBytes = inputLayout->elementStrides[binding.slot];
                VBVs[binding.slot].SizeInBytes = (UINT)(std::min(buffer->desc.byteSize - binding.offset, (uint64_t)ULONG_MAX));
                VBVs[binding.slot].BufferLocation = buffer->gpuVA + binding.offset;
                maxVbIndex = std::max(maxVbIndex, binding.slot);

                m_instance->referencedResources.push_back(buffer);
            }

            if (m_currentGraphicsStateValid)
            {
                for (const VertexBufferBinding& binding : m_currentGraphicsState.vertexBuffers)
                {
                    if (binding.slot < c_MaxVertexAttributes)
                        maxVbIndex = std::max(maxVbIndex, binding.slot);
                }
            }

            m_activeCommandList->commandList->IASetVertexBuffers(0, maxVbIndex + 1, VBVs);
        }

        if (m_enableAutomaticBarriers && state.indexBuffer.buffer && (m_bindingStatesDirty || updateVertexBuffers))
        {
            for (const VertexBufferBinding& binding : state.vertexBuffers)
            {
                Buffer* buffer = checked_cast<Buffer*>(binding.buffer);

                requireBufferState(buffer, ResourceStates::VertexBuffer);
            }
        }

        if (updateShadingRate || updateFramebuffer)
        {
            const auto& framebufferDesc = framebuffer->getDesc();
            bool shouldEnableVariableRateShading = framebufferDesc.shadingRateAttachment.valid() && state.shadingRateState.enabled;
            bool variableRateShadingCurrentlyEnabled = m_currentGraphicsStateValid
                && m_currentGraphicsState.framebuffer->getDesc().shadingRateAttachment.valid() && m_currentGraphicsState.shadingRateState.enabled;

            if (shouldEnableVariableRateShading)
            {
                Texture* texture = checked_cast<Texture*>(framebufferDesc.shadingRateAttachment.texture);
                m_activeCommandList->commandList6->RSSetShadingRateImage(texture->resource);
            }
            else if (variableRateShadingCurrentlyEnabled)
            {
                // shading rate attachment is not enabled in framebuffer, or VRS is turned off, so unbind VRS image
                m_activeCommandList->commandList6->RSSetShadingRateImage(nullptr);
            }
        }

        if (updateShadingRate)
        {
            if (state.shadingRateState.enabled)
            {
                static_assert(D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT == 2);
                D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT];
                combiners[0] = convertShadingRateCombiner(state.shadingRateState.pipelinePrimitiveCombiner);
                combiners[1] = convertShadingRateCombiner(state.shadingRateState.imageCombiner);
                m_activeCommandList->commandList6->RSSetShadingRate(convertPixelShadingRate(state.shadingRateState.shadingRate), combiners);
            }
            else if (m_currentGraphicsStateValid && m_currentGraphicsState.shadingRateState.enabled)
            {
                // only call if the old state had VRS enabled and we need to disable it
                m_activeCommandList->commandList6->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
            }
        }

        commitBarriers();

        if (updateViewports)
        {
            DX12_ViewportState vpState = convertViewportState(pso->desc.renderState.rasterState, framebuffer->framebufferInfo, state.viewport);

            if (vpState.numViewports)
            {
                m_activeCommandList->commandList->RSSetViewports(vpState.numViewports, vpState.viewports);
            }

            if (vpState.numScissorRects)
            {
                m_activeCommandList->commandList->RSSetScissorRects(vpState.numScissorRects, vpState.scissorRects);
            }
        }

#if CAUSTICA_RHI_D3D12_WITH_NVAPI
        bool updateSPS = m_currentSinglePassStereoState != pso->desc.renderState.singlePassStereo;

        if (updateSPS)
        {
            const SinglePassStereoState& spsState = pso->desc.renderState.singlePassStereo;

            NvAPI_Status Status = NvAPI_D3D12_SetSinglePassStereoMode(m_activeCommandList->commandList, spsState.enabled ? 2 : 1, spsState.renderTargetIndexOffset, spsState.independentViewportMask);

            if (Status != NVAPI_OK)
            {
                m_context.error("NvAPI_D3D12_SetSinglePassStereoMode call failed");
            }

            m_currentSinglePassStereoState = spsState;
        }
#endif

        m_currentGraphicsStateValid = true;
        m_currentComputeStateValid = false;
        m_currentMeshletStateValid = false;
        m_currentRayTracingStateValid = false;
        m_currentGraphicsState = state;
        m_currentGraphicsState.dynamicStencilRefValue = effectiveStencilRefValue;
        m_bindingStatesDirty = false;
    }

    void CommandList::unbindShadingRateState()
    {
        if (m_currentGraphicsStateValid && m_currentGraphicsState.shadingRateState.enabled)
        {
            m_activeCommandList->commandList6->RSSetShadingRateImage(nullptr);
            m_activeCommandList->commandList6->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
            m_currentGraphicsState.shadingRateState.enabled = false;
            m_currentGraphicsState.framebuffer = nullptr;
        }
    }


    void CommandList::updateGraphicsVolatileBuffers()
    {
        // If there are some volatile buffers bound, and they have been written into since the last draw or setGraphicsState, patch their views
        if (!m_anyVolatileBufferWrites)
            return;

        for (VolatileConstantBufferBinding& parameter : m_currentGraphicsVolatileCBs)
        {
            D3D12_GPU_VIRTUAL_ADDRESS currentGpuVA = m_volatileConstantBufferAddresses[parameter.buffer];

            if (currentGpuVA != parameter.address)
            {
                m_activeCommandList->commandList->SetGraphicsRootConstantBufferView(parameter.bindingPoint, currentGpuVA);

                parameter.address = currentGpuVA;
            }
        }

        m_anyVolatileBufferWrites = false;
    }

    void CommandList::bindGraphicsPipeline(GraphicsPipeline *pso, bool updateRootSignature) const
    {
        const auto& pipelineDesc = pso->desc;

        if (updateRootSignature)
        {
            m_activeCommandList->commandList->SetGraphicsRootSignature(pso->rootSignature->handle);
        }

        m_activeCommandList->commandList->SetPipelineState(pso->pipelineState);

        m_activeCommandList->commandList->IASetPrimitiveTopology(convertPrimitiveType(pipelineDesc.primType, pipelineDesc.patchControlPoints));
    }

    void CommandList::draw(const DrawArguments& args)
    {
        updateGraphicsVolatileBuffers();

        m_activeCommandList->commandList->DrawInstanced(args.vertexCount, args.instanceCount, args.startVertexLocation, args.startInstanceLocation);
    }

    void CommandList::drawIndexed(const DrawArguments& args)
    {
        updateGraphicsVolatileBuffers();

        m_activeCommandList->commandList->DrawIndexedInstanced(args.vertexCount, args.instanceCount, args.startIndexLocation, args.startVertexLocation, args.startInstanceLocation);
    }

    void CommandList::drawIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        Buffer* indirectParams = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        assert(indirectParams); // validation layer handles this

        updateGraphicsVolatileBuffers();

        m_activeCommandList->commandList->ExecuteIndirect(m_context.drawIndirectSignature, drawCount, indirectParams->resource, offsetBytes, nullptr, 0);
    }

    void CommandList::drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        Buffer* indirectParams = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        assert(indirectParams);

        updateGraphicsVolatileBuffers();

        m_activeCommandList->commandList->ExecuteIndirect(m_context.drawIndexedIndirectSignature, drawCount, indirectParams->resource, offsetBytes, nullptr, 0);
    }

    void CommandList::drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount)
    {
        Buffer* paramBuffer = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        Buffer* countBuffer = checked_cast<Buffer*>(m_currentGraphicsState.indirectCountBuffer);
        assert(paramBuffer);
        assert(countBuffer);

        updateGraphicsVolatileBuffers();

        m_activeCommandList->commandList->ExecuteIndirect(
            m_context.drawIndexedIndirectSignature,
            maxDrawCount,
            paramBuffer->resource,
            paramOffsetBytes,
            countBuffer->resource,
            countOffsetBytes
        );
    }

    DX12_ViewportState convertViewportState(const RasterState& rasterState, const FramebufferInfoEx& framebufferInfo, const ViewportState& vpState)
    {
        DX12_ViewportState ret;

        ret.numViewports = UINT(vpState.viewports.size());
        for (size_t rt = 0; rt < vpState.viewports.size(); rt++)
        {
            ret.viewports[rt].TopLeftX = vpState.viewports[rt].minX;
            ret.viewports[rt].TopLeftY = vpState.viewports[rt].minY;
            ret.viewports[rt].Width = vpState.viewports[rt].maxX - vpState.viewports[rt].minX;
            ret.viewports[rt].Height = vpState.viewports[rt].maxY - vpState.viewports[rt].minY;
            ret.viewports[rt].MinDepth = vpState.viewports[rt].minZ;
            ret.viewports[rt].MaxDepth = vpState.viewports[rt].maxZ;
        }

        ret.numScissorRects = UINT(vpState.scissorRects.size());
        for(size_t rt = 0; rt < vpState.scissorRects.size(); rt++)
        {
            if (rasterState.scissorEnable)
            {
                ret.scissorRects[rt].left = (LONG)vpState.scissorRects[rt].minX;
                ret.scissorRects[rt].top = (LONG)vpState.scissorRects[rt].minY;
                ret.scissorRects[rt].right = (LONG)vpState.scissorRects[rt].maxX;
                ret.scissorRects[rt].bottom = (LONG)vpState.scissorRects[rt].maxY;
            }
            else
            {
                ret.scissorRects[rt].left = (LONG)vpState.viewports[rt].minX;
                ret.scissorRects[rt].top = (LONG)vpState.viewports[rt].minY;
                ret.scissorRects[rt].right = (LONG)vpState.viewports[rt].maxX;
                ret.scissorRects[rt].bottom = (LONG)vpState.viewports[rt].maxY;

                if (framebufferInfo.width > 0)
                {
                    ret.scissorRects[rt].left = std::max(ret.scissorRects[rt].left, LONG(0));
                    ret.scissorRects[rt].top = std::max(ret.scissorRects[rt].top, LONG(0));
                    ret.scissorRects[rt].right = std::min(ret.scissorRects[rt].right, LONG(framebufferInfo.width));
                    ret.scissorRects[rt].bottom = std::min(ret.scissorRects[rt].bottom, LONG(framebufferInfo.height));
                }
            }
        }

        return ret;
    }

    void TranslateBlendState(const BlendState& inState, D3D12_BLEND_DESC& outState)
    {
        outState.AlphaToCoverageEnable = inState.alphaToCoverageEnable;
        outState.IndependentBlendEnable = true;

        for (uint32_t i = 0; i < c_MaxRenderTargets; i++)
        {
            const BlendState::RenderTarget& src = inState.targets[i];
            D3D12_RENDER_TARGET_BLEND_DESC& dst = outState.RenderTarget[i];

            dst.BlendEnable = src.blendEnable ? TRUE : FALSE;
            dst.SrcBlend = convertBlendValue(src.srcBlend);
            dst.DestBlend = convertBlendValue(src.destBlend);
            dst.BlendOp = convertBlendOp(src.blendOp);
            dst.SrcBlendAlpha = convertBlendValue(src.srcBlendAlpha);
            dst.DestBlendAlpha = convertBlendValue(src.destBlendAlpha);
            dst.BlendOpAlpha = convertBlendOp(src.blendOpAlpha);
            dst.RenderTargetWriteMask = (D3D12_COLOR_WRITE_ENABLE)src.colorWriteMask;
        }
    }

    void TranslateDepthStencilState(const DepthStencilState& inState, D3D12_DEPTH_STENCIL_DESC& outState)
    {
        outState.DepthEnable = inState.depthTestEnable ? TRUE : FALSE;
        outState.DepthWriteMask = inState.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        outState.DepthFunc = convertComparisonFunc(inState.depthFunc);
        outState.StencilEnable = inState.stencilEnable ? TRUE : FALSE;
        outState.StencilReadMask = (UINT8)inState.stencilReadMask;
        outState.StencilWriteMask = (UINT8)inState.stencilWriteMask;
        outState.FrontFace.StencilFailOp = convertStencilOp(inState.frontFaceStencil.failOp);
        outState.FrontFace.StencilDepthFailOp = convertStencilOp(inState.frontFaceStencil.depthFailOp);
        outState.FrontFace.StencilPassOp = convertStencilOp(inState.frontFaceStencil.passOp);
        outState.FrontFace.StencilFunc = convertComparisonFunc(inState.frontFaceStencil.stencilFunc);
        outState.BackFace.StencilFailOp = convertStencilOp(inState.backFaceStencil.failOp);
        outState.BackFace.StencilDepthFailOp = convertStencilOp(inState.backFaceStencil.depthFailOp);
        outState.BackFace.StencilPassOp = convertStencilOp(inState.backFaceStencil.passOp);
        outState.BackFace.StencilFunc = convertComparisonFunc(inState.backFaceStencil.stencilFunc);
    }

    void TranslateRasterizerState(const RasterState& inState, D3D12_RASTERIZER_DESC& outState)
    {
        switch (inState.fillMode)
        {
        case RasterFillMode::Solid:
            outState.FillMode = D3D12_FILL_MODE_SOLID;
            break;
        case RasterFillMode::Wireframe:
            outState.FillMode = D3D12_FILL_MODE_WIREFRAME;
            break;
        default:
            utils::InvalidEnum();
            break;
        }

        switch (inState.cullMode)
        {
        case RasterCullMode::Back:
            outState.CullMode = D3D12_CULL_MODE_BACK;
            break;
        case RasterCullMode::Front:
            outState.CullMode = D3D12_CULL_MODE_FRONT;
            break;
        case RasterCullMode::None:
            outState.CullMode = D3D12_CULL_MODE_NONE;
            break;
        default:
            utils::InvalidEnum();
            break;
        }

        outState.FrontCounterClockwise = inState.frontCounterClockwise ? TRUE : FALSE;
        outState.DepthBias = inState.depthBias;
        outState.DepthBiasClamp = inState.depthBiasClamp;
        outState.SlopeScaledDepthBias = inState.slopeScaledDepthBias;
        outState.DepthClipEnable = inState.depthClipEnable ? TRUE : FALSE;
        outState.MultisampleEnable = inState.multisampleEnable ? TRUE : FALSE;
        outState.AntialiasedLineEnable = inState.antialiasedLineEnable ? TRUE : FALSE;
        outState.ConservativeRaster = inState.conservativeRasterEnable ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        outState.ForcedSampleCount = inState.forcedSampleCount;
    }

} // namespace caustica::rhi::d3d12
