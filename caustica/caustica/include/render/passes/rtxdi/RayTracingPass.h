#pragma once

#include <rhi/nvrhi.h>

namespace caustica
{
    class ShaderFactory;
    struct ShaderMacro;
}


struct RayTracingPass
{
    nvrhi::ShaderHandle ComputeShader;
    nvrhi::ComputePipelineHandle ComputePipeline;

    nvrhi::ShaderLibraryHandle ShaderLibrary;
    nvrhi::rt::PipelineHandle RayTracingPipeline;
    nvrhi::rt::ShaderTableHandle ShaderTable;

    uint32_t ComputeGroupSize = 0;

    bool init(
        nvrhi::IDevice* device,
        caustica::ShaderFactory& shaderFactory,
        const char* shaderName,
        const std::vector<caustica::ShaderMacro>& extraMacros,
        bool useRayQuery,
        uint32_t computeGroupSize,
        nvrhi::IBindingLayout* bindingLayout,
        nvrhi::IBindingLayout* extraBindingLayout = nullptr,
        nvrhi::IBindingLayout* bindlessLayout = nullptr);

    void execute(
        nvrhi::ICommandList* commandList,
        int width,
        int height,
        nvrhi::IBindingSet* bindingSet,
        nvrhi::IBindingSet* extraBindingSet,
        nvrhi::IDescriptorTable* descriptorTable,
        const void* pushConstants = nullptr,
        size_t pushConstantSize = 0);
};