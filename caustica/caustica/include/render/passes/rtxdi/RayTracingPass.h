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
        caustica::rhi::IDevice* device,
        caustica::ShaderFactory& shaderFactory,
        const char* shaderName,
        const std::vector<caustica::ShaderMacro>& extraMacros,
        bool useRayQuery,
        uint32_t computeGroupSize,
        caustica::rhi::IBindingLayout* bindingLayout,
        caustica::rhi::IBindingLayout* extraBindingLayout = nullptr,
        caustica::rhi::IBindingLayout* bindlessLayout = nullptr);

    void execute(
        caustica::rhi::ICommandList* commandList,
        int width,
        int height,
        caustica::rhi::IBindingSet* bindingSet,
        caustica::rhi::IBindingSet* extraBindingSet,
        caustica::rhi::IDescriptorTable* descriptorTable,
        const void* pushConstants = nullptr,
        size_t pushConstantSize = 0);
};