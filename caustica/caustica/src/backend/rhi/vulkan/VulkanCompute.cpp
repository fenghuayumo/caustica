#include "VulkanBackend.h"
#include <rhi/common/misc.h>

namespace caustica::rhi::vulkan
{
    ComputePipelineHandle Device::createComputePipeline(const ComputePipelineDesc& desc)
    {
        vk::Result res;

        assert(desc.CS);

        ComputePipeline *pso = new ComputePipeline(m_context);
        pso->desc = desc;

        res = createPipelineLayout(
            pso->pipelineLayout,
            pso->pipelineBindingLayouts,
            pso->pushConstantVisibility,
            pso->descriptorSetIdxToBindingIdx,
            m_context,
            desc.bindingLayouts);
        CHECK_VK_FAIL(res)

        Shader* CS = checked_cast<Shader*>(desc.CS.Get());

        // See createGraphicsPipeline() for a more expanded implementation
        // of shader specializations with multiple shaders in the pipeline

        size_t numShaders = 0;
        size_t numShadersWithSpecializations = 0;
        size_t numSpecializationConstants = 0;

        countSpecializationConstants(CS, numShaders, numShadersWithSpecializations, numSpecializationConstants);

        assert(numShaders == 1);

        std::vector<vk::SpecializationInfo> specInfos;
        std::vector<vk::SpecializationMapEntry> specMapEntries;
        std::vector<uint32_t> specData;

        specInfos.reserve(numShadersWithSpecializations);
        specMapEntries.reserve(numSpecializationConstants);
        specData.reserve(numSpecializationConstants);

        auto shaderStageInfo = makeShaderStageCreateInfo(CS,
            specInfos, specMapEntries, specData);

        auto pipelineInfo = vk::ComputePipelineCreateInfo()
                                .setStage(shaderStageInfo)
                                .setLayout(pso->pipelineLayout);

        res = m_context.device.createComputePipelines(m_context.pipelineCache,
                                                    1, &pipelineInfo,
                                                    m_context.allocationCallbacks,
                                                    &pso->pipeline);

        if (res != vk::Result::eSuccess)
            m_context.error(std::string("Failed to create Vulkan compute pipeline: ") + resultToString(VkResult(res)));
        CHECK_VK_FAIL(res)

        return ComputePipelineHandle::Create(pso);
    }

    ComputePipeline::~ComputePipeline()
    {
        if (pipeline)
        {
            m_context.device.destroyPipeline(pipeline, m_context.allocationCallbacks);
            pipeline = nullptr;
        }

        if (pipelineLayout)
        {
            m_context.device.destroyPipelineLayout(pipelineLayout, m_context.allocationCallbacks);
            pipelineLayout = nullptr;
        }
    }

    Object ComputePipeline::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::VK_PipelineLayout:
            return Object(pipelineLayout);
        case ObjectTypes::VK_Pipeline:
            return Object(pipeline);
        default:
            return nullptr;
        }
    }

    void CommandList::setComputeState(const ComputeState& state)
    {
        endRenderPass();

        assert(m_currentCmdBuf);

        ComputePipeline* pso = checked_cast<ComputePipeline*>(state.pipeline);

        if (m_enableAutomaticBarriers)
        {
            insertComputeResourceBarriers(state);
        }

        if (m_currentComputeState.pipeline != state.pipeline)
        {
            m_currentCmdBuf->cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, pso->pipeline);

            m_currentCmdBuf->referencedResources.push_back(state.pipeline);
        }

        if (state.indirectParams && state.indirectParams != m_currentComputeState.indirectParams)
        {
            m_currentCmdBuf->referencedResources.push_back(state.indirectParams);
        }

        if (arraysAreDifferent(m_currentComputeState.bindings, state.bindings) || m_anyVolatileBufferWrites)
        {
            bindBindingSets(vk::PipelineBindPoint::eCompute, pso->pipelineLayout, state.bindings, pso->descriptorSetIdxToBindingIdx);
        }

        m_currentPipelineLayout = pso->pipelineLayout;
        m_currentPushConstantsVisibility = pso->pushConstantVisibility;

        commitBarriers();

        m_currentGraphicsState = GraphicsState();
        m_currentComputeState = state;
        m_currentMeshletState = MeshletState();
        m_currentRayTracingState = rt::State();
        m_anyVolatileBufferWrites = false;
    }

    void CommandList::updateComputeVolatileBuffers()
    {
        if (m_anyVolatileBufferWrites && m_currentComputeState.pipeline)
        {
            ComputePipeline* pso = checked_cast<ComputePipeline*>(m_currentComputeState.pipeline);

            bindBindingSets(vk::PipelineBindPoint::eCompute, pso->pipelineLayout, m_currentComputeState.bindings, pso->descriptorSetIdxToBindingIdx);

            m_anyVolatileBufferWrites = false;
        }
    }

    void CommandList::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        assert(m_currentCmdBuf);

        updateComputeVolatileBuffers();

        m_currentCmdBuf->cmdBuf.dispatch(groupsX, groupsY, groupsZ);
    }

    void CommandList::dispatchIndirect(uint32_t offsetBytes)
    {
        assert(m_currentCmdBuf);

        updateComputeVolatileBuffers();

        Buffer* indirectParams = checked_cast<Buffer*>(m_currentComputeState.indirectParams);
        assert(indirectParams);

        m_currentCmdBuf->cmdBuf.dispatchIndirect(indirectParams->buffer, offsetBytes);
    }

} // namespace caustica::rhi::vulkan
