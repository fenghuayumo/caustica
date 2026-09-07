#include "D3D12Backend.h"

#include <rhi/common/misc.h>
#include <iomanip>
#include <sstream>

namespace caustica::rhi::d3d12
{
    Object ComputePipeline::getNativeObject(ObjectType objectType)
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


    RefCountPtr<ID3D12PipelineState> Device::createPipelineState(const ComputePipelineDesc & state, RootSignature* pRS) const
    {
        RefCountPtr<ID3D12PipelineState> pipelineState;

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};

        desc.pRootSignature = pRS->handle;
        Shader* shader = checked_cast<Shader*>(state.CS.Get());
        desc.CS = { &shader->bytecode[0], shader->bytecode.size() };

#if CAUSTICA_RHI_D3D12_WITH_NVAPI
        if (!shader->extensions.empty())
        {
            NvAPI_Status status = NvAPI_D3D12_CreateComputePipelineState(m_context.device, &desc,
                NvU32(shader->extensions.size()), const_cast<const NVAPI_D3D12_PSO_EXTENSION_DESC**>(shader->extensions.data()), &pipelineState);

            if (status != NVAPI_OK || pipelineState == nullptr)
            {
                m_context.error("Failed to create a compute pipeline state object with NVAPI extensions");
                return nullptr;
            }

            return pipelineState;
        }
#endif

        const HRESULT hr = m_context.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelineState));

        if (FAILED(hr))
        {
            std::ostringstream ss;
            ss << "Failed to create a compute pipeline state object, HRESULT = 0x" << std::hex << std::setw(8) << hr
               << ", shader = '" << shader->desc.debugName << "', bytecode = " << std::dec << shader->bytecode.size();
            m_context.error(ss.str());
            return nullptr;
        }

        return pipelineState;
    }

    ComputePipelineHandle Device::createComputePipeline(const ComputePipelineDesc& desc)
    {
        RefCountPtr<RootSignature> pRS = getRootSignature(desc.bindingLayouts, false);
        RefCountPtr<ID3D12PipelineState> pPSO = createPipelineState(desc, pRS);

        if (pPSO == nullptr)
            return nullptr;

        ComputePipeline *pso = new ComputePipeline();

        pso->desc = desc;

        pso->rootSignature = pRS;
        pso->pipelineState = pPSO;

        return ComputePipelineHandle::Create(pso);
    }

    void CommandList::setComputeState(const ComputeState& state)
    {
        ComputePipeline* pso = checked_cast<ComputePipeline*>(state.pipeline);

        const bool updateRootSignature = !m_currentComputeStateValid || m_currentComputeState.pipeline == nullptr ||
            checked_cast<ComputePipeline*>(m_currentComputeState.pipeline)->rootSignature != pso->rootSignature;

        bool updatePipeline = !m_currentComputeStateValid || m_currentComputeState.pipeline != state.pipeline;
        bool updateIndirectParams = !m_currentComputeStateValid || m_currentComputeState.indirectParams != state.indirectParams;

        uint32_t bindingUpdateMask = 0;
        if (!m_currentComputeStateValid || updateRootSignature)
            bindingUpdateMask = ~0u;

        if (commitDescriptorHeaps())
            bindingUpdateMask = ~0u;

        if (bindingUpdateMask == 0)
            bindingUpdateMask = arrayDifferenceMask(m_currentComputeState.bindings, state.bindings);

        if (updateRootSignature)
        {
            m_activeCommandList->commandList->SetComputeRootSignature(pso->rootSignature->handle);
        }

        if (updatePipeline)
        {
            m_activeCommandList->commandList->SetPipelineState(pso->pipelineState);

            m_instance->referencedResources.push_back(pso);
        }

        setComputeBindings(state.bindings, bindingUpdateMask, checked_cast<Buffer*>(state.indirectParams), updateIndirectParams, pso->rootSignature);

        unbindShadingRateState();

        m_currentGraphicsStateValid = false;
        m_currentComputeStateValid = true;
        m_currentMeshletStateValid = false;
        m_currentRayTracingStateValid = false;
        m_currentComputeState = state;
        m_bindingStatesDirty = false;

        commitBarriers();
    }

    void CommandList::updateComputeVolatileBuffers()
    {
        // If there are some volatile buffers bound, and they have been written into since the last dispatch or setComputeState, patch their views
        if (!m_anyVolatileBufferWrites)
            return;

        for (VolatileConstantBufferBinding& parameter : m_currentComputeVolatileCBs)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS currentGpuVA = m_volatileConstantBufferAddresses[parameter.buffer];

            if (currentGpuVA != parameter.address)
            {
                m_activeCommandList->commandList->SetComputeRootConstantBufferView(parameter.bindingPoint, currentGpuVA);

                parameter.address = currentGpuVA;
            }
        }

        m_anyVolatileBufferWrites = false;
    }

    void CommandList::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        updateComputeVolatileBuffers();

        m_activeCommandList->commandList->Dispatch(groupsX, groupsY, groupsZ);
    }

    void CommandList::dispatchIndirect(uint32_t offsetBytes)
    {
        Buffer* indirectParams = checked_cast<Buffer*>(m_currentComputeState.indirectParams);
        assert(indirectParams); // validation layer handles this

        updateComputeVolatileBuffers();

        m_activeCommandList->commandList->ExecuteIndirect(m_context.dispatchIndirectSignature, 1, indirectParams->resource, offsetBytes, nullptr, 0);
    }

} // namespace caustica::rhi::d3d12
