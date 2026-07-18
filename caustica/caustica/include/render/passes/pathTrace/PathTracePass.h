#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

class PTPipelineVariant;

namespace caustica::render
{

// Dispatch helpers for path-trace graph passes. Pipelines / PSOs live on WorldRenderer
// (and are copied into FrameGraphContext); this type owns no GPU state.
class PathTracePass
{
public:
    void prePass(
        nvrhi::ICommandList* commandList,
        nvrhi::BindingSetHandle bindingSet,
        nvrhi::IDescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        PTPipelineVariant* pipeline);

    void exportVBuffer(
        nvrhi::ICommandList* commandList,
        nvrhi::BindingSetHandle bindingSet,
        nvrhi::IDescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        nvrhi::IComputePipeline* pipeline);

    void mainPass(
        nvrhi::ICommandList* commandList,
        nvrhi::BindingSetHandle bindingSet,
        nvrhi::IDescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        PTPipelineVariant* pipeline,
        uint32_t samplesPerPixel);
};

} // namespace caustica::render
