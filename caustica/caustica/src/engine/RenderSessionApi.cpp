#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/RenderSessionApi.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/RenderTargets.h>
#include <math/math.h>

using namespace caustica::render;

namespace caustica
{

void debugDrawLine(App& app, float3 start, float3 stop, float4 col1, float4 col2)
{
    auto* wr = worldRenderer(app);
    if (!wr)
        return;
    auto& lines = wr->getCpuSideDebugLines();
    if (int(lines.size()) + 2 >= MAX_DEBUG_LINES)
        return;
    DebugLineStruct dls = { float4(start, 1), col1 };
    DebugLineStruct dle = { float4(stop, 1), col2 };
    lines.push_back(dls);
    lines.push_back(dle);
}

const std::string& envMapLocalPath(const App& app) { return gpuRender(app)->lightingPasses().envMapLocalPath(); }

const std::string& envMapOverrideSource(const App& app) { return gpuRender(app)->lightingPasses().envMapOverride(); }

const std::vector<std::filesystem::path>& envMapMediaList(App& app) { return gpuRender(app)->lightingPasses().envMapMediaList(); }

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride)
{
    gpuRender(app)->lightingPasses().setEnvMapOverrideSource(envMapOverride);
}

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return gpuRender(app)->gaussianSplatPasses().loadFromFile(fileName, convertRdfToRub);
}

uint32_t gaussianSplatCount(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().splatCount();
}

uint32_t gaussianSplatObjectCount(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().objectCount();
}

const std::string& gaussianSplatFileName(const App& app)
{
    return gpuRender(app)->gaussianSplatPasses().fileNameSummary();
}

void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work)
{
    if (!work)
        return;
    app.runGpuWorkOnRenderThread(work);
}

std::string resolutionInfo(const App& app)
{
    auto* wr = worldRenderer(app);
    if (!wr)
        return "uninitialized";
    const auto* targets = wr->getRenderTargets();
    if (targets == nullptr || targets->outputColor == nullptr)
        return "uninitialized";
    const auto rs = wr->getRenderSize();
    const auto ds = wr->getDisplaySize();
    if (dm::all(rs == ds))
        return std::to_string(rs.x) + "x" + std::to_string(rs.y);
    return std::to_string(rs.x) + "x" + std::to_string(rs.y)
        + "->" + std::to_string(ds.x) + "x" + std::to_string(ds.y);
}

float avgTimePerFrame(const App& app)
{
    AppDiagnostics* diag = diagnostics(app);
    if (!diag || diag->benchFrames == 0)
        return 0.0f;
    std::chrono::duration<double> elapsed = (diag->benchLast - diag->benchStart);
    return float(elapsed.count() / diag->benchFrames);
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh)
{
    requestMeshAccelRebuild(app, mesh, true);
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation)
{
    gpuRender(app)->rayTracingResources().requestMeshAccelRebuild(mesh, resetAccumulation);
}

nvrhi::ITexture* ldrColorTexture(const App& app)
{
    auto* wr = worldRenderer(app);
    const auto* targets = wr ? wr->getRenderTargets() : nullptr;
    return targets ? targets->ldrColor.Get() : nullptr;
}

const DebugFeedbackStruct& feedbackData(const App& app)
{
    static const DebugFeedbackStruct kEmpty{};
    auto* wr = worldRenderer(app);
    return wr ? wr->getFeedbackData() : kEmpty;
}

const DeltaTreeVizPathVertex* debugDeltaPathTree(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getDebugDeltaPathTree() : nullptr;
}

int accumulationSampleIndex(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getAccumulationSampleIndex() : 0;
}

math::uint2 renderSize(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getRenderSize() : uint2{ 0, 0 };
}

math::uint2 displaySize(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->getDisplaySize() : uint2{ 0, 0 };
}

bool accumulationCompleted(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr && wr->getAccumulationCompleted();
}

std::string fpsInfo(const App& app)
{
    if (SceneViewState* vs = viewState(app))
        return vs->fpsInfo;
    return {};
}

} // namespace caustica
