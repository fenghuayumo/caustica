#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

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
        nvrhi::IDevice* device,
        caustica::ShaderFactory* shaderFactory,
        nvrhi::BindingLayoutHandle bindingLayout,
        nvrhi::BindingLayoutHandle bindlessLayout);

    [[nodiscard]] nvrhi::ComputePipelineHandle exportVBufferPSO() const { return m_exportVBufferPSO; }

    void fillConstants(
        PathTracerConstants& constants,
        const PathTracerCameraData& cameraData,
        const FillConstantsParams& params) const;

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

private:
    nvrhi::ShaderHandle m_exportVBufferCS;
    nvrhi::ComputePipelineHandle m_exportVBufferPSO;
};

} // namespace caustica::render
