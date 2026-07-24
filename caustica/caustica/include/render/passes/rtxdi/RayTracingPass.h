#pragma once

#include <rhi/rhi.h>

namespace caustica
{
    class ShaderFactory;
    struct ShaderMacro;
}


struct RayTracingPass
{
    caustica::rhi::ShaderHandle ComputeShader;
    caustica::rhi::ComputePipelineHandle ComputePipeline;

    caustica::rhi::ShaderLibraryHandle ShaderLibrary;
    caustica::rhi::rt::PipelineHandle RayTracingPipeline;
    caustica::rhi::rt::ShaderTableHandle ShaderTable;

    uint32_t ComputeGroupSize = 0;

    bool init(
        caustica::rhi::Device* device,
        caustica::ShaderFactory& shaderFactory,
        const char* shaderName,
        const std::vector<caustica::ShaderMacro>& extraMacros,
        bool useRayQuery,
        uint32_t computeGroupSize,
        caustica::rhi::BindingLayout* bindingLayout,
        caustica::rhi::BindingLayout* extraBindingLayout = nullptr,
        caustica::rhi::BindingLayout* bindlessLayout = nullptr);

    void execute(
        caustica::rhi::CommandList* commandList,
        int width,
        int height,
        caustica::rhi::BindingSet* bindingSet,
        caustica::rhi::BindingSet* extraBindingSet,
        caustica::rhi::DescriptorTable* descriptorTable,
        const void* pushConstants = nullptr,
        size_t pushConstantSize = 0);
};