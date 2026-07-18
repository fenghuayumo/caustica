#include <render/passes/rtxdi/RtxdiGraphResources.h>

#include <render/core/PathTracerSettings.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/rtxdi/RtxdiResources.h>

namespace caustica::render
{

bool tryImportRtxdiGraphResources(
    rg::GraphBuilder& graph,
    RtxdiPass* rtxdiPass,
    RtxdiGraphResources& outResources)
{
    if (rtxdiPass == nullptr)
        return false;

    const std::shared_ptr<RtxdiResources> resources = rtxdiPass->getRTXDIResources();
    if (resources == nullptr)
        return false;

    outResources.risBuffer = graph.importBuffer(resources->RisBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.lightDataBuffer = graph.importBuffer(resources->LightDataBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.risLightDataBuffer = graph.importBuffer(resources->RisLightDataBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.lightReservoirBuffer = graph.importBuffer(resources->LightReservoirBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.giReservoirBuffer = graph.importBuffer(resources->GIReservoirBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.ptReservoirBuffer = graph.importBuffer(resources->PTReservoirBuffer, rg::BufferAccess::UnorderedAccess);
    outResources.localLightPdf = graph.importTexture(resources->LocalLightPdfTexture, rg::TextureAccess::UnorderedAccess);
    return true;
}

void declareRtxdiBeginFrameAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources,
    const PathTraceGraphTargets& /*pathTraceTargets*/)
{
    setup.write(rtxdiResources.risBuffer, rg::BufferAccess::UnorderedAccess);
    setup.write(rtxdiResources.risLightDataBuffer, rg::BufferAccess::UnorderedAccess);
    setup.write(rtxdiResources.lightDataBuffer, rg::BufferAccess::UnorderedAccess);
    setup.write(rtxdiResources.localLightPdf, rg::TextureAccess::UnorderedAccess);
}

void declareRtxdiExecuteAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources,
    const PathTraceGraphTargets& pathTraceTargets,
    const PathTracerSettings& settings)
{
    declarePathTraceOutputWrites(setup, pathTraceTargets);
    setup.write(pathTraceTargets.secondarySurfacePositionNormal, rg::TextureAccess::UnorderedAccess);
    setup.write(pathTraceTargets.secondarySurfaceRadiance, rg::TextureAccess::UnorderedAccess);

    setup.read(rtxdiResources.risBuffer, rg::BufferAccess::UnorderedAccess);
    setup.read(rtxdiResources.lightDataBuffer, rg::BufferAccess::UnorderedAccess);
    setup.write(rtxdiResources.lightReservoirBuffer, rg::BufferAccess::UnorderedAccess);

    if (settings.actualUseReSTIRGI())
        setup.write(rtxdiResources.giReservoirBuffer, rg::BufferAccess::UnorderedAccess);

    if (settings.actualUseReSTIRPT())
        setup.write(rtxdiResources.ptReservoirBuffer, rg::BufferAccess::UnorderedAccess);
}

} // namespace caustica::render
