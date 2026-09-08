#include "VulkanBackend.h"
#include <rhi/common/misc.h>

namespace caustica::rhi::vulkan
{
    static TextureDimension getDimensionForFramebuffer(TextureDimension dimension, bool isArray)
    {
        // Can't render into cubes and 3D textures directly, convert them to 2D arrays
        if (dimension == TextureDimension::TextureCube || dimension == TextureDimension::TextureCubeArray || dimension == TextureDimension::Texture3D)
            dimension = TextureDimension::Texture2DArray;

        if (!isArray)
        {
            // Demote arrays to single textures if we just need one layer
            switch(dimension)  // NOLINT(clang-diagnostic-switch-enum)
            {
            case TextureDimension::Texture1DArray:
                dimension = TextureDimension::Texture1D;
                break;
            case TextureDimension::Texture2DArray:
                dimension = TextureDimension::Texture2D;
                break;
            case TextureDimension::Texture2DMSArray:
                dimension = TextureDimension::Texture2DMS;
                break;
            default:
                break;
            }
        }

        return dimension;
    }

    FramebufferHandle Device::createFramebuffer(const FramebufferDesc& desc)
    {
        Framebuffer *fb = new Framebuffer();
        fb->desc = desc;
        fb->framebufferInfo = FramebufferInfoEx(desc);

        for(uint32_t i = 0; i < desc.colorAttachments.size(); i++)
        {
            const auto& rt = desc.colorAttachments[i];
            Texture* t = checked_cast<Texture*>(rt.texture);

            assert(fb->framebufferInfo.width == std::max(t->desc.width >> rt.subresources.baseMipLevel, 1u));
            assert(fb->framebufferInfo.height == std::max(t->desc.height >> rt.subresources.baseMipLevel, 1u));

            TextureSubresourceSet subresources = rt.subresources.resolve(t->desc, true);

            TextureDimension dimension = getDimensionForFramebuffer(t->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = t->getSubresourceView(subresources, dimension, rt.format, vk::ImageUsageFlagBits::eColorAttachment);

            vk::RenderingAttachmentInfo& attachmentInfo = fb->colorAttachments.emplace_back();
            attachmentInfo = vk::RenderingAttachmentInfo()
                .setImageView(view.view)
                .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
                .setLoadOp(vk::AttachmentLoadOp::eLoad)
                .setStoreOp(vk::AttachmentStoreOp::eStore);

            fb->resources.push_back(rt.texture);
        }

        // add depth/stencil attachment if present
        if (desc.depthAttachment.valid())
        {
            const auto& att = desc.depthAttachment;

            Texture* texture = checked_cast<Texture*>(att.texture);

            assert(fb->framebufferInfo.width == std::max(texture->desc.width >> att.subresources.baseMipLevel, 1u));
            assert(fb->framebufferInfo.height == std::max(texture->desc.height >> att.subresources.baseMipLevel, 1u));

            vk::ImageLayout depthLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
            if (desc.depthAttachment.isReadOnly)
            {
                depthLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            }

            TextureSubresourceSet subresources = att.subresources.resolve(texture->desc, true);

            TextureDimension dimension = getDimensionForFramebuffer(texture->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = texture->getSubresourceView(subresources, dimension, att.format, vk::ImageUsageFlagBits::eDepthStencilAttachment);

            fb->depthAttachment = vk::RenderingAttachmentInfo()
                .setImageView(view.view)
                .setImageLayout(depthLayout)
                .setLoadOp(vk::AttachmentLoadOp::eLoad)
                .setStoreOp(vk::AttachmentStoreOp::eStore);

            if (getFormatInfo(texture->desc.format).hasStencil)
                fb->stencilAttachment = fb->depthAttachment;

            fb->resources.push_back(att.texture);
        }

        // add VRS attachment
        if (desc.shadingRateAttachment.valid())
        {
            const auto& vrsAttachment = desc.shadingRateAttachment;
            Texture* vrsTexture = checked_cast<Texture*>(vrsAttachment.texture);
            assert(vrsTexture->imageInfo.format == vk::Format::eR8Uint);
            assert(vrsTexture->imageInfo.samples == vk::SampleCountFlagBits::e1);

            TextureSubresourceSet subresources = vrsAttachment.subresources.resolve(vrsTexture->desc, true);
            TextureDimension dimension = getDimensionForFramebuffer(vrsTexture->desc.dimension, subresources.numArraySlices > 1);

            const auto& view = vrsTexture->getSubresourceView(subresources, dimension, vrsAttachment.format, vk::ImageUsageFlagBits::eFragmentShadingRateAttachmentKHR);

            auto rateProps = vk::PhysicalDeviceFragmentShadingRatePropertiesKHR();
            auto props = vk::PhysicalDeviceProperties2();
            props.pNext = &rateProps;
            m_context.physicalDevice.getProperties2(&props);

            fb->shadingRateAttachment = vk::RenderingFragmentShadingRateAttachmentInfoKHR()
                .setImageLayout(vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR)
                .setImageView(view.view)
                .setShadingRateAttachmentTexelSize(rateProps.minFragmentShadingRateAttachmentTexelSize);

            fb->resources.push_back(vrsAttachment.texture);
        }

        return FramebufferHandle::Create(fb);
    }

    void countSpecializationConstants(
        Shader* shader,
        size_t& numShaders,
        size_t& numShadersWithSpecializations,
        size_t& numSpecializationConstants)
    {
        if (!shader)
            return;

        numShaders += 1;

        if (shader->specializationConstants.empty())
            return;

        numShadersWithSpecializations += 1;
        numSpecializationConstants += shader->specializationConstants.size();
    }

    vk::PipelineShaderStageCreateInfo makeShaderStageCreateInfo(
        Shader *shader,
        std::vector<vk::SpecializationInfo>& specInfos,
        std::vector<vk::SpecializationMapEntry>& specMapEntries,
        std::vector<uint32_t>& specData)
    {
        auto shaderStageCreateInfo = vk::PipelineShaderStageCreateInfo()
            .setStage(shader->stageFlagBits)
            .setModule(shader->shaderModule)
            .setPName(shader->desc.entryName.c_str());

        if (!shader->specializationConstants.empty())
        {
            // For specializations, this functions allocates:
            //  - One entry in specInfos per shader
            //  - One entry in specMapEntries and specData each per constant
            // The vectors are pre-allocated, so it's safe to use .data() before writing the data

            assert(specInfos.data());
            assert(specMapEntries.data());
            assert(specData.data());

            shaderStageCreateInfo.setPSpecializationInfo(specInfos.data() + specInfos.size());

            auto specInfo = vk::SpecializationInfo()
                .setPMapEntries(specMapEntries.data() + specMapEntries.size())
                .setMapEntryCount(static_cast<uint32_t>(shader->specializationConstants.size()))
                .setPData(specData.data() + specData.size())
                .setDataSize(shader->specializationConstants.size() * sizeof(uint32_t));

            size_t dataOffset = 0;
            for (const auto& constant : shader->specializationConstants)
            {
                auto specMapEntry = vk::SpecializationMapEntry()
                    .setConstantID(constant.constantID)
                    .setOffset(static_cast<uint32_t>(dataOffset))
                    .setSize(sizeof(uint32_t));

                specMapEntries.push_back(specMapEntry);
                specData.push_back(constant.value.u);
                dataOffset += specMapEntry.size;
            }

            specInfos.push_back(specInfo);
        }

        return shaderStageCreateInfo;
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo)
    {
        if (desc.renderState.singlePassStereo.enabled)
        {
            m_context.error("Single-pass stereo is not supported by the Vulkan backend");
            return nullptr;
        }

        vk::Result res;

        InputLayout* inputLayout = checked_cast<InputLayout*>(desc.inputLayout.Get());

        GraphicsPipeline *pso = new GraphicsPipeline(m_context);
        pso->desc = desc;
        pso->framebufferInfo = fbinfo;

        Shader* VS = checked_cast<Shader*>(desc.VS.Get());
        Shader* HS = checked_cast<Shader*>(desc.HS.Get());
        Shader* DS = checked_cast<Shader*>(desc.DS.Get());
        Shader* GS = checked_cast<Shader*>(desc.GS.Get());
        Shader* PS = checked_cast<Shader*>(desc.PS.Get());

        size_t numShaders = 0;
        size_t numShadersWithSpecializations = 0;
        size_t numSpecializationConstants = 0;

        // Count the spec constants for all stages
        countSpecializationConstants(VS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(HS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(DS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(GS, numShaders, numShadersWithSpecializations, numSpecializationConstants);
        countSpecializationConstants(PS, numShaders, numShadersWithSpecializations, numSpecializationConstants);

        std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;
        std::vector<vk::SpecializationInfo> specInfos;
        std::vector<vk::SpecializationMapEntry> specMapEntries;
        std::vector<uint32_t> specData;

        // Allocate buffers for specialization constants and related structures
        // so that shaderStageCreateInfo(...) can directly use pointers inside the vectors
        // because the vectors won't reallocate their buffers
        shaderStages.reserve(numShaders);
        specInfos.reserve(numShadersWithSpecializations);
        specMapEntries.reserve(numSpecializationConstants);
        specData.reserve(numSpecializationConstants);

        // Set up shader stages
        if (desc.VS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(VS,
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Vertex;
        }

        if (desc.HS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(HS,
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Hull;
        }

        if (desc.DS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(DS,
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Domain;
        }

        if (desc.GS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(GS,
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Geometry;
        }

        if (desc.PS)
        {
            shaderStages.push_back(makeShaderStageCreateInfo(PS,
                specInfos, specMapEntries, specData));
            pso->shaderMask = pso->shaderMask | ShaderType::Pixel;
        }

        // set up vertex input state
        auto vertexInput = vk::PipelineVertexInputStateCreateInfo();
        if (inputLayout)
        {
            vertexInput.setVertexBindingDescriptionCount(uint32_t(inputLayout->bindingDesc.size()))
                       .setPVertexBindingDescriptions(inputLayout->bindingDesc.data())
                       .setVertexAttributeDescriptionCount(uint32_t(inputLayout->attributeDesc.size()))
                       .setPVertexAttributeDescriptions(inputLayout->attributeDesc.data());
        }

        auto inputAssembly = vk::PipelineInputAssemblyStateCreateInfo()
                                .setTopology(convertPrimitiveTopology(desc.primType));

        // fixed function state
        const auto& rasterState = desc.renderState.rasterState;
        const auto& depthStencilState = desc.renderState.depthStencilState;
        const auto& blendState = desc.renderState.blendState;

        auto viewportState = vk::PipelineViewportStateCreateInfo()
            .setViewportCount(1)
            .setScissorCount(1);

        auto rasterizer = vk::PipelineRasterizationStateCreateInfo()
                            // .setDepthClampEnable(??)
                            // .setRasterizerDiscardEnable(??)
                            .setPolygonMode(convertFillMode(rasterState.fillMode))
                            .setCullMode(convertCullMode(rasterState.cullMode))
                            .setFrontFace(rasterState.frontCounterClockwise ?
                                            vk::FrontFace::eCounterClockwise : vk::FrontFace::eClockwise)
                            .setDepthBiasEnable(rasterState.depthBias ? true : false)
                            .setDepthBiasConstantFactor(float(rasterState.depthBias))
                            .setDepthBiasClamp(rasterState.depthBiasClamp)
                            .setDepthBiasSlopeFactor(rasterState.slopeScaledDepthBias)
                            .setLineWidth(1.0f);

        // Conservative raster state
        auto conservativeRasterState = vk::PipelineRasterizationConservativeStateCreateInfoEXT()
            .setConservativeRasterizationMode(vk::ConservativeRasterizationModeEXT::eOverestimate);
		if (rasterState.conservativeRasterEnable)
		{
			rasterizer.setPNext(&conservativeRasterState);
		}

        auto multisample = vk::PipelineMultisampleStateCreateInfo()
                            .setRasterizationSamples(vk::SampleCountFlagBits(fbinfo.sampleCount))
                            .setAlphaToCoverageEnable(blendState.alphaToCoverageEnable);

        auto depthStencil = vk::PipelineDepthStencilStateCreateInfo()
                                .setDepthTestEnable(depthStencilState.depthTestEnable)
                                .setDepthWriteEnable(depthStencilState.depthWriteEnable)
                                .setDepthCompareOp(convertCompareOp(depthStencilState.depthFunc))
                                .setStencilTestEnable(depthStencilState.stencilEnable)
                                .setFront(convertStencilState(depthStencilState, depthStencilState.frontFaceStencil))
                                .setBack(convertStencilState(depthStencilState, depthStencilState.backFaceStencil));

        // VRS state
        std::array<vk::FragmentShadingRateCombinerOpKHR, 2> combiners = {
            convertShadingRateCombiner(desc.shadingRateState.pipelinePrimitiveCombiner),
            convertShadingRateCombiner(desc.shadingRateState.imageCombiner)
        };
        auto shadingRateState = vk::PipelineFragmentShadingRateStateCreateInfoKHR()
            .setCombinerOps(combiners)
            .setFragmentSize(convertFragmentShadingRate(desc.shadingRateState.shadingRate));

        res = createPipelineLayout(
            pso->pipelineLayout,
            pso->pipelineBindingLayouts,
            pso->pushConstantVisibility,
            pso->descriptorSetIdxToBindingIdx,
            m_context,
            desc.bindingLayouts);
        CHECK_VK_FAIL(res)

        static_vector<vk::PipelineColorBlendAttachmentState, c_MaxRenderTargets> colorBlendAttachments(fbinfo.colorFormats.size());

        for(uint32_t i = 0; i < uint32_t(fbinfo.colorFormats.size()); i++)
        {
            colorBlendAttachments[i] = convertBlendState(blendState.targets[i]);
        }

        auto colorBlend = vk::PipelineColorBlendStateCreateInfo()
                            .setAttachmentCount(uint32_t(colorBlendAttachments.size()))
                            .setPAttachments(colorBlendAttachments.data());

        pso->usesBlendConstants = blendState.usesConstantColor(uint32_t(fbinfo.colorFormats.size()));

        static_vector<vk::DynamicState, 5> dynamicStates = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor
        };
        if (pso->usesBlendConstants)
            dynamicStates.push_back(vk::DynamicState::eBlendConstants);
        if (pso->desc.renderState.depthStencilState.dynamicStencilRef)
            dynamicStates.push_back(vk::DynamicState::eStencilReference);
        if (pso->desc.shadingRateState.enabled)
            dynamicStates.push_back(vk::DynamicState::eFragmentShadingRateKHR);

        auto dynamicStateInfo = vk::PipelineDynamicStateCreateInfo()
            .setDynamicStateCount(uint32_t(dynamicStates.size()))
            .setPDynamicStates(dynamicStates.data());

        std::array<vk::Format, c_MaxRenderTargets> colorFormats;
        for (size_t i = 0; i < fbinfo.colorFormats.size(); i++)
            colorFormats[i] = vk::Format(convertFormat(fbinfo.colorFormats[i]));

        FormatInfo const& depthStencilFormatInfo = getFormatInfo(fbinfo.depthFormat);
        vk::Format depthStencilFormat = vk::Format(convertFormat(fbinfo.depthFormat));

        auto renderingInfo = vk::PipelineRenderingCreateInfo()
            .setColorAttachmentCount(uint32_t(fbinfo.colorFormats.size()))
            .setPColorAttachmentFormats(colorFormats.data())
            .setDepthAttachmentFormat(depthStencilFormatInfo.hasDepth ? depthStencilFormat : vk::Format::eUndefined)
            .setStencilAttachmentFormat(depthStencilFormatInfo.hasStencil ? depthStencilFormat : vk::Format::eUndefined);

        auto pipelineInfo = vk::GraphicsPipelineCreateInfo()
            .setPNext(&renderingInfo)
            .setStageCount(uint32_t(shaderStages.size()))
            .setPStages(shaderStages.data())
            .setPVertexInputState(&vertexInput)
            .setPInputAssemblyState(&inputAssembly)
            .setPViewportState(&viewportState)
            .setPRasterizationState(&rasterizer)
            .setPMultisampleState(&multisample)
            .setPDepthStencilState(&depthStencil)
            .setPColorBlendState(&colorBlend)
            .setPDynamicState(&dynamicStateInfo)
            .setLayout(pso->pipelineLayout)
            .setBasePipelineHandle(nullptr)
            .setBasePipelineIndex(-1)
            .setPTessellationState(nullptr);

        if (pso->desc.shadingRateState.enabled)
            renderingInfo.setPNext(&shadingRateState);

        auto tessellationState = vk::PipelineTessellationStateCreateInfo();

        if (desc.primType == PrimitiveType::PatchList)
        {
            tessellationState.setPatchControlPoints(desc.patchControlPoints);
            pipelineInfo.setPTessellationState(&tessellationState);
        }

        res = m_context.device.createGraphicsPipelines(m_context.pipelineCache,
                                                     1, &pipelineInfo,
                                                     m_context.allocationCallbacks,
                                                     &pso->pipeline);
        ASSERT_VK_OK(res); // for debugging
        CHECK_VK_FAIL(res);

        return GraphicsPipelineHandle::Create(pso);
    }

    GraphicsPipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc, rhi::Framebuffer* fb)
    {
        if (!fb)
            return nullptr;

        return createGraphicsPipeline(desc, fb->getFramebufferInfo());
    }

    GraphicsPipeline::~GraphicsPipeline()
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

    Object GraphicsPipeline::getNativeObject(ObjectType objectType)
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

    void CommandList::beginRenderPass(caustica::rhi::Framebuffer* _framebuffer)
    {
        endRenderPass();

        Framebuffer* framebuffer = checked_cast<Framebuffer*>(_framebuffer);

        if (!framebuffer)
            return;

        m_currentGraphicsState.framebuffer = framebuffer;
        m_currentMeshletState.framebuffer = framebuffer;

        vk::RenderingInfo renderingInfo = vk::RenderingInfo()
            .setRenderArea(vk::Rect2D()
                .setOffset(vk::Offset2D(0, 0))
                .setExtent(vk::Extent2D(framebuffer->framebufferInfo.width, framebuffer->framebufferInfo.height)))
            .setLayerCount(framebuffer->framebufferInfo.arraySize)
            .setColorAttachmentCount(uint32_t(framebuffer->colorAttachments.size()))
            .setPColorAttachments(framebuffer->colorAttachments.data())
            .setPDepthAttachment(framebuffer->depthAttachment.imageView ? &framebuffer->depthAttachment : nullptr)
            .setPStencilAttachment(framebuffer->stencilAttachment.imageView ? &framebuffer->stencilAttachment : nullptr);

        m_currentCmdBuf->cmdBuf.beginRendering(renderingInfo);
        m_currentCmdBuf->referencedResources.push_back(framebuffer);
    }

    void CommandList::endRenderPass()
    {
        if (m_currentGraphicsState.framebuffer || m_currentMeshletState.framebuffer)
        {
            m_currentCmdBuf->cmdBuf.endRendering();
            m_currentGraphicsState.framebuffer = nullptr;
            m_currentMeshletState.framebuffer = nullptr;
        }
    }

    static vk::Viewport VKViewportWithDXCoords(const Viewport& v)
    {
        return vk::Viewport(v.minX, v.maxY, v.maxX - v.minX, -(v.maxY - v.minY), v.minZ, v.maxZ);
    }

    void CommandList::setGraphicsState(const GraphicsState& state)
    {
        assert(m_currentCmdBuf);

        GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(state.pipeline);
        Framebuffer* fb = checked_cast<Framebuffer*>(state.framebuffer);

        if (m_enableAutomaticBarriers)
        {
            insertGraphicsResourceBarriers(state);
        }

        bool anyBarriers = this->anyBarriers();
        bool updatePipeline = false;

        if (m_currentGraphicsState.pipeline != state.pipeline)
        {
            m_currentCmdBuf->cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pso->pipeline);

            m_currentCmdBuf->referencedResources.push_back(state.pipeline);
            updatePipeline = true;
        }

        if (m_currentGraphicsState.framebuffer != state.framebuffer || anyBarriers /* because barriers cannot be set inside a renderpass */)
        {
            endRenderPass();
        }

        commitBarriers();

        if(!m_currentGraphicsState.framebuffer)
        {
            beginRenderPass(fb);
        }

        m_currentPipelineLayout = pso->pipelineLayout;
        m_currentPushConstantsVisibility = pso->pushConstantVisibility;

        if (arraysAreDifferent(m_currentGraphicsState.bindings, state.bindings) || m_anyVolatileBufferWrites)
        {
            bindBindingSets(vk::PipelineBindPoint::eGraphics, pso->pipelineLayout, state.bindings, pso->descriptorSetIdxToBindingIdx);
        }

        if (!state.viewport.viewports.empty() && arraysAreDifferent(state.viewport.viewports, m_currentGraphicsState.viewport.viewports))
        {
            caustica::rhi::static_vector<vk::Viewport, c_MaxViewports> viewports;
            for (const auto& vp : state.viewport.viewports)
            {
                viewports.push_back(VKViewportWithDXCoords(vp));
            }

            m_currentCmdBuf->cmdBuf.setViewport(0, uint32_t(viewports.size()), viewports.data());
        }

        if (!state.viewport.scissorRects.empty() && arraysAreDifferent(state.viewport.scissorRects, m_currentGraphicsState.viewport.scissorRects))
        {
            caustica::rhi::static_vector<vk::Rect2D, c_MaxViewports> scissors;
            for (const auto& sc : state.viewport.scissorRects)
            {
                scissors.push_back(vk::Rect2D(vk::Offset2D(sc.minX, sc.minY),
                    vk::Extent2D(std::abs(sc.maxX - sc.minX), std::abs(sc.maxY - sc.minY))));
            }

            m_currentCmdBuf->cmdBuf.setScissor(0, uint32_t(scissors.size()), scissors.data());
        }

        if (pso->desc.renderState.depthStencilState.dynamicStencilRef && (updatePipeline || m_currentGraphicsState.dynamicStencilRefValue != state.dynamicStencilRefValue))
        {
            m_currentCmdBuf->cmdBuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, state.dynamicStencilRefValue);
        }

        if (pso->usesBlendConstants && (updatePipeline || m_currentGraphicsState.blendConstantColor != state.blendConstantColor))
        {
            m_currentCmdBuf->cmdBuf.setBlendConstants(&state.blendConstantColor.r);
        }

        if (state.indexBuffer.buffer && m_currentGraphicsState.indexBuffer != state.indexBuffer)
        {
            m_currentCmdBuf->cmdBuf.bindIndexBuffer(checked_cast<Buffer*>(state.indexBuffer.buffer)->buffer,
                state.indexBuffer.offset,
                state.indexBuffer.format == Format::R16_UINT ?
                vk::IndexType::eUint16 : vk::IndexType::eUint32);

            m_currentCmdBuf->referencedResources.push_back(state.indexBuffer.buffer);
        }

        if (!state.vertexBuffers.empty() && arraysAreDifferent(state.vertexBuffers, m_currentGraphicsState.vertexBuffers))
        {
            vk::Buffer vertexBuffers[c_MaxVertexAttributes];
            vk::DeviceSize vertexBufferOffsets[c_MaxVertexAttributes];
            uint32_t maxVbIndex = 0;

            for (const auto& binding : state.vertexBuffers)
            {
                // This is tested by the validation layer, skip invalid slots here if VL is not used.
                if (binding.slot >= c_MaxVertexAttributes)
                    continue;

                vertexBuffers[binding.slot] = checked_cast<Buffer*>(binding.buffer)->buffer;
                vertexBufferOffsets[binding.slot] = vk::DeviceSize(binding.offset);
                maxVbIndex = std::max(maxVbIndex, binding.slot);

                m_currentCmdBuf->referencedResources.push_back(binding.buffer);
            }

            m_currentCmdBuf->cmdBuf.bindVertexBuffers(0, maxVbIndex + 1, vertexBuffers, vertexBufferOffsets);
        }

        if (state.indirectParams)
        {
            m_currentCmdBuf->referencedResources.push_back(state.indirectParams);
        }

        if (state.indirectCountBuffer && state.indirectCountBuffer != state.indirectParams)
        {
            m_currentCmdBuf->referencedResources.push_back(state.indirectCountBuffer);
        }

        if (state.shadingRateState.enabled)
        {
            vk::FragmentShadingRateCombinerOpKHR combiners[2] = { convertShadingRateCombiner(state.shadingRateState.pipelinePrimitiveCombiner), convertShadingRateCombiner(state.shadingRateState.imageCombiner) };
            vk::Extent2D shadingRate = convertFragmentShadingRate(state.shadingRateState.shadingRate);
            m_currentCmdBuf->cmdBuf.setFragmentShadingRateKHR(&shadingRate, combiners);
        }

        m_currentGraphicsState = state;
        m_currentComputeState = ComputeState();
        m_currentMeshletState = MeshletState();
        m_currentRayTracingState = rt::State();
        m_anyVolatileBufferWrites = false;
    }

    void CommandList::updateGraphicsVolatileBuffers()
    {
        if (m_anyVolatileBufferWrites && m_currentGraphicsState.pipeline)
        {
            GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(m_currentGraphicsState.pipeline);

            bindBindingSets(vk::PipelineBindPoint::eGraphics, pso->pipelineLayout, m_currentGraphicsState.bindings, pso->descriptorSetIdxToBindingIdx);

            m_anyVolatileBufferWrites = false;
        }
    }

    void CommandList::draw(const DrawArguments& args)
    {
        assert(m_currentCmdBuf);

        updateGraphicsVolatileBuffers();

        m_currentCmdBuf->cmdBuf.draw(args.vertexCount,
            args.instanceCount,
            args.startVertexLocation,
            args.startInstanceLocation);
    }

    void CommandList::drawIndexed(const DrawArguments& args)
    {
        assert(m_currentCmdBuf);

        updateGraphicsVolatileBuffers();

        m_currentCmdBuf->cmdBuf.drawIndexed(args.vertexCount,
            args.instanceCount,
            args.startIndexLocation,
            args.startVertexLocation,
            args.startInstanceLocation);
    }

    void CommandList::drawIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        assert(m_currentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* indirectParams = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        assert(indirectParams);

        m_currentCmdBuf->cmdBuf.drawIndirect(indirectParams->buffer, offsetBytes, drawCount, sizeof(DrawIndirectArguments));
    }

    void CommandList::drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount)
    {
        assert(m_currentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* indirectParams = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        assert(indirectParams);

        m_currentCmdBuf->cmdBuf.drawIndexedIndirect(indirectParams->buffer, offsetBytes, drawCount, sizeof(DrawIndexedIndirectArguments));
    }

    void CommandList::drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount)
    {
        assert(m_currentCmdBuf);

        updateGraphicsVolatileBuffers();

        Buffer* paramBuffer = checked_cast<Buffer*>(m_currentGraphicsState.indirectParams);
        Buffer* countBuffer = checked_cast<Buffer*>(m_currentGraphicsState.indirectCountBuffer);
        assert(paramBuffer);
        assert(countBuffer);

        m_currentCmdBuf->cmdBuf.drawIndexedIndirectCount(
            paramBuffer->buffer,
            paramOffsetBytes,
            countBuffer->buffer,
            countOffsetBytes,
            maxDrawCount,
            sizeof(DrawIndexedIndirectArguments)
        );
    }

} // namespace caustica::rhi::vulkan
