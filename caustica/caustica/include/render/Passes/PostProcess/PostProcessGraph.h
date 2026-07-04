#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/Core/PathTracerSettings.h>
#include <math/math.h>
#include <rhi/nvrhi.h>

class RenderTargets;
class PTPipelineVariant;
class ToneMappingPass;

namespace caustica
{
class CameraController;
class ICompositeView;
}

namespace caustica::render
{
class BloomPass;
}

namespace caustica::rg
{

struct PostProcessGraphParams
{
    GraphBuilder& graph;

    RenderTargets*              renderTargets = nullptr;
    PathTracerSettings*         settings = nullptr;
    caustica::CameraController* camera = nullptr;
    render::BloomPass*          bloomPass = nullptr;
    ToneMappingPass*            toneMappingPass = nullptr;
    const caustica::ICompositeView* compositeView = nullptr;
    dm::uint2                   displaySize{};

    nvrhi::BindingSetHandle     pathTracingBindingSet;
    nvrhi::IDescriptorTable*    descriptorTable = nullptr;
    PTPipelineVariant*          testRaygenPpHdrPipeline = nullptr;
    PTPipelineVariant*          edgeDetectionPipeline = nullptr;

    bool* outCommandListWasClosed = nullptr;
};

void buildPostProcessGraph(const PostProcessGraphParams& params);

} // namespace caustica::rg
