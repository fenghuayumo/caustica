#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneGaussianSplatLogic.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/RenderSessionApi.h>
#include <engine/EnqueueRenderCommand.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/WorldRenderer.h>
#include <render/core/RenderTargets.h>
#include <render/passes/debug/ZoomTool.h>
#include <assets/loader/TextureLoader.h>
#include <backend/GpuDevice.h>
#include <math/math.h>
#include <cstdint>

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

const std::string& envMapLocalPath(const App& app) { return worldRenderer(app)->lightingPasses().envMapLocalPath(); }

const std::string& envMapOverrideSource(const App& app) { return worldRenderer(app)->lightingPasses().envMapOverride(); }

const std::vector<std::filesystem::path>& envMapMediaList(App& app) { return worldRenderer(app)->lightingPasses().envMapMediaList(); }

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride)
{
    worldRenderer(app)->lightingPasses().setEnvMapOverrideSource(envMapOverride);
}

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub)
{
    PathTracerSettings* cfg = settings(app);
    WorldRenderer* renderer = worldRenderer(app);
    if (!cfg || !renderer)
        return false;

    return SceneGaussianSplatLogic::loadFromFile(
        renderer->gaussianSplatPasses(), *cfg, fileName, app, convertRdfToRub);
}

uint32_t gaussianSplatCount(const App& app)
{
    return worldRenderer(app)->gaussianSplatPasses().splatCount();
}

uint32_t gaussianSplatObjectCount(const App& app)
{
    return worldRenderer(app)->gaussianSplatPasses().objectCount();
}

const std::string& gaussianSplatFileName(const App& app)
{
    return worldRenderer(app)->gaussianSplatPasses().fileNameSummary();
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
    if (math::all(rs == ds))
        return std::to_string(rs.x) + "x" + std::to_string(rs.y);
    return std::to_string(rs.x) + "x" + std::to_string(rs.y)
        + "->" + std::to_string(ds.x) + "x" + std::to_string(ds.y);
}

float avgTimePerFrame(const App& app)
{
    AppDiagnostics* diag = diagnostics(app);
    return diag ? diag->averageBenchmarkFrameSeconds() : 0.0f;
}

void requestMeshAccelRebuild(App& app, ecs::Entity entity, bool resetAccumulation)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    scene::SceneEntityWorld* ew = scene ? scene->getEntityWorld() : nullptr;
    if (!ew || !ecs::isValid(entity))
        return;
    auto* meshInstance = ew->world().tryGet<scene::MeshInstanceComponent>(entity);
    if (!meshInstance || !meshInstance->mesh)
        return;
    requestMeshAccelRebuild(app, meshInstance->mesh, resetAccumulation);
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh)
{
    requestMeshAccelRebuild(app, mesh, true);
}

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation)
{
    worldRenderer(app)->rayTracingResources().requestMeshAccelRebuild(mesh, resetAccumulation);
}

caustica::rhi::Texture* ldrColorTexture(const App& app)
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

uint32_t precacheRtFeaturePresets(App& app, bool showProgress)
{
    auto* wr = worldRenderer(app);
    if (!wr)
        return 0;
    uint32_t ready = 0;
    // THREADING: Logic↔RT wait — ADR 0002 S5 remaining tool path (sync return count
    // for Python/host); not on the interactive frame loop.
    EnqueueRenderCommandAndWait(app, [wr, showProgress, &ready]() {
        ready = wr->precacheAllRtFeaturePresets(showProgress);
    });
    return ready;
}

void requestFullAccelRebuild(App& app)
{
    if (auto* wr = worldRenderer(app))
        wr->rayTracingResources().requestAccelerationStructureRebuild();
}

uint32_t renderFrameIndex(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? static_cast<uint32_t>(wr->getFrameIndex()) : 0;
}

void setGaussianSplatTemporalReset(App& app, bool enabled)
{
    if (auto* wr = worldRenderer(app))
        wr->setGaussianSplatTemporalReset(enabled);
}

bool takeDenoisedScreenshot(App& app, caustica::rhi::Texture* target)
{
    auto* wr = worldRenderer(app);
    if (!wr || !target)
        return false;
    wr->denoisedScreenshot(target);
    return true;
}

std::shared_ptr<LightSamplingCache> lightSamplingCache(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->lightingPasses().lightSampling() : nullptr;
}

std::shared_ptr<EnvMapProcessor> envMapProcessor(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->lightingPasses().environment() : nullptr;
}

std::shared_ptr<OpacityMicromapBuilder> opacityMicromapBuilder(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->lightingPasses().opacityMaps() : nullptr;
}

std::shared_ptr<MaterialGpuCache> materialGpuCache(const App& app)
{
    auto* wr = worldRenderer(app);
    return wr ? wr->lightingPasses().materials() : nullptr;
}

void saveAllMaterials(App& app)
{
    if (auto materials = materialGpuCache(app))
        materials->saveAll();
}

void submitImmediateMaterialPick(App& app, const render::RenderPickState& picking)
{
    if (auto* wr = worldRenderer(app))
        wr->submitImmediateMaterialPick(picking);
}

void submitImmediateInstancePick(App& app, const render::RenderPickState& picking)
{
    if (auto* wr = worldRenderer(app))
        wr->submitImmediateInstancePick(picking);
}

const render::RenderPickState& lastRenderedPicking(const App& app)
{
    static const render::RenderPickState kEmpty{};
    auto* wr = worldRenderer(app);
    return wr ? wr->getLastRenderedPicking() : kEmpty;
}

std::vector<GaussianSplatObjectBounds> gaussianSplatObjectBounds(const App& app)
{
    std::vector<GaussianSplatObjectBounds> out;
    auto* wr = worldRenderer(app);
    if (!wr)
        return out;
    for (const auto& object : wr->gaussianSplatPasses().objects())
    {
        if (!ecs::isValid(object.entity) || !object.pass)
            continue;
        out.push_back({ object.entity, object.pass->getLocalBounds() });
    }
    return out;
}

const scene::SceneRenderData* latestPublishedRenderData(const App& app)
{
    auto scene = activeScene(app);
    if (!scene)
        return nullptr;
    const uint32_t frame = scene->latestPublishedRenderFrameIndex();
    if (frame == UINT32_MAX)
        return nullptr;
    return &scene->getRenderDataForFrame(frame);
}

std::shared_ptr<ShaderFactory> shaderFactory(const App& app)
{
    auto* infra = gpuSharedCaches(app);
    return infra ? infra->shaderFactory : nullptr;
}

std::unique_ptr<ZoomTool> createZoomTool(App& app)
{
    auto factory = shaderFactory(app);
    auto* device = gpuDevice(app);
    if (!factory || !device)
        return nullptr;
    return std::make_unique<ZoomTool>(device->getDevice(), std::move(factory));
}

bool saveCurrentFramebuffer(App& app, GpuDevice& gpuDevice, const char* fileName)
{
    caustica::rhi::Framebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
    auto* infra = gpuSharedCaches(app);
    if (!framebuffer || !infra || !infra->renderDevice || !fileName)
        return false;
    caustica::rhi::Texture* texture = framebuffer->getDesc().colorAttachments[0].texture;
    return saveTextureToFile(
        gpuDevice.getDevice(),
        *infra->renderDevice,
        texture,
        caustica::rhi::ResourceStates::Common,
        fileName);
}

} // namespace caustica
