#pragma once

// Public API — session/frame queries and a few render controls (see docs/public-api.md).
// Prefer entity overloads; MeshInfo overloads are for engine/editor internals.

#include <ecs/Entity.h>
#include <math/math.h>

#include <render/RenderRuntimeState.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rhi/rhi.h>

struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;
class LightSamplingCache;
class EnvMapProcessor;
class OpacityMicromapBuilder;
class MaterialGpuCache;
class ZoomTool;

namespace caustica
{
class GpuDevice;
}

namespace caustica::scene
{
class SceneRenderData;
}

namespace caustica
{

class App;
struct MeshInfo;

struct GaussianSplatObjectBounds
{
    ecs::Entity entity = ecs::NullEntity;
    math::box3 localBounds = math::box3::empty();
};

void debugDrawLine(App& app, math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride);
[[nodiscard]] const std::string& envMapLocalPath(const App& app);
[[nodiscard]] const std::string& envMapOverrideSource(const App& app);
[[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList(App& app);

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub = true);
[[nodiscard]] uint32_t gaussianSplatCount(const App& app);
[[nodiscard]] uint32_t gaussianSplatObjectCount(const App& app);
[[nodiscard]] const std::string& gaussianSplatFileName(const App& app);

// Prefer entity for public/host code. MeshInfo overloads are engine-internal use.
void requestMeshAccelRebuild(App& app, ecs::Entity entity, bool resetAccumulation = true);
void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh);
void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation);

[[nodiscard]] caustica::rhi::Texture* ldrColorTexture(const App& app);
[[nodiscard]] const DebugFeedbackStruct& feedbackData(const App& app);
[[nodiscard]] const DeltaTreeVizPathVertex* debugDeltaPathTree(const App& app);
[[nodiscard]] int accumulationSampleIndex(const App& app);
[[nodiscard]] math::uint2 renderSize(const App& app);
[[nodiscard]] math::uint2 displaySize(const App& app);
[[nodiscard]] bool accumulationCompleted(const App& app);
[[nodiscard]] float avgTimePerFrame(const App& app);
[[nodiscard]] std::string resolutionInfo(const App& app);
[[nodiscard]] std::string fpsInfo(const App& app);

uint32_t precacheRtFeaturePresets(App& app, bool showProgress = true);
void requestFullAccelRebuild(App& app);
[[nodiscard]] uint32_t renderFrameIndex(const App& app);
void setGaussianSplatTemporalReset(App& app, bool enabled = true);
bool takeDenoisedScreenshot(App& app, caustica::rhi::Texture* target);

[[nodiscard]] std::shared_ptr<LightSamplingCache> lightSamplingCache(const App& app);
[[nodiscard]] std::shared_ptr<EnvMapProcessor> envMapProcessor(const App& app);
[[nodiscard]] std::shared_ptr<OpacityMicromapBuilder> opacityMicromapBuilder(const App& app);
[[nodiscard]] std::shared_ptr<MaterialGpuCache> materialGpuCache(const App& app);
void saveAllMaterials(App& app);

void submitImmediateMaterialPick(App& app, const render::RenderPickState& picking);
void submitImmediateInstancePick(App& app, const render::RenderPickState& picking);
[[nodiscard]] const render::RenderPickState& lastRenderedPicking(const App& app);

[[nodiscard]] std::vector<GaussianSplatObjectBounds> gaussianSplatObjectBounds(const App& app);

[[nodiscard]] const scene::SceneRenderData* latestPublishedRenderData(const App& app);
[[nodiscard]] std::unique_ptr<ZoomTool> createZoomTool(App& app);
bool saveCurrentFramebuffer(App& app, GpuDevice& gpuDevice, const char* fileName);

} // namespace caustica
