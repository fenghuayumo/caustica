#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

#include <memory>

class PTPipelineVariant;
class RenderTargets;
class ToneMappingPass;
struct PathTracerCameraData;
struct PathTracerConstants;

namespace caustica
{
class ShaderFactory;
}

namespace caustica::render
{

class PathTracingContext;

// Path-trace dispatch helpers + export VBuffer PSO / constants fill.
class PathTracePass
{
public:
    struct FillConstantsParams
    {
        PathTracingContext* context = nullptr;
        ToneMappingPass* toneMapping = nullptr;
        const RenderTargets* renderTargets = nullptr;
        dm::uint2 renderSize{};
        dm::uint2 displaySize{};
        uint32_t sampleIndex = 0;
        uint64_t frameIndex = 0;
    };

    bool createExportPipeline(
        caustica::rhi::Device* device,
        caustica::ShaderFactory* shaderFactory,
        caustica::rhi::BindingLayoutHandle bindingLayout,
        caustica::rhi::BindingLayoutHandle bindlessLayout);

    [[nodiscard]] caustica::rhi::ComputePipelineHandle exportVBufferPSO() const { return m_exportVBufferPSO; }

    void fillConstants(
        PathTracerConstants& constants,
        const PathTracerCameraData& cameraData,
        const FillConstantsParams& params) const;

    void prePass(
        caustica::rhi::CommandList* commandList,
        caustica::rhi::BindingSetHandle bindingSet,
        caustica::rhi::DescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        PTPipelineVariant* pipeline);

    void exportVBuffer(
        caustica::rhi::CommandList* commandList,
        caustica::rhi::BindingSetHandle bindingSet,
        caustica::rhi::DescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        caustica::rhi::ComputePipeline* pipeline);

    void mainPass(
        caustica::rhi::CommandList* commandList,
        caustica::rhi::BindingSetHandle bindingSet,
        caustica::rhi::DescriptorTable* descriptorTable,
        dm::uint2 viewSize,
        PTPipelineVariant* pipeline,
        uint32_t samplesPerPixel);

private:
    caustica::rhi::ShaderHandle m_exportVBufferCS;
    caustica::rhi::ComputePipelineHandle m_exportVBufferPSO;
};

} // namespace caustica::render
