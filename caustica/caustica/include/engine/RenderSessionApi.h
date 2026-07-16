#pragma once

#include <math/math.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rhi/nvrhi.h>

struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;

namespace caustica
{

class App;
struct MeshInfo;

void debugDrawLine(App& app, math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);
void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work);

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride);
[[nodiscard]] const std::string& envMapLocalPath(const App& app);
[[nodiscard]] const std::string& envMapOverrideSource(const App& app);
[[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList(App& app);

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub = true);
[[nodiscard]] uint32_t gaussianSplatCount(const App& app);
[[nodiscard]] uint32_t gaussianSplatObjectCount(const App& app);
[[nodiscard]] const std::string& gaussianSplatFileName(const App& app);

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh);
void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation);

[[nodiscard]] nvrhi::ITexture* ldrColorTexture(const App& app);
[[nodiscard]] const DebugFeedbackStruct& feedbackData(const App& app);
[[nodiscard]] const DeltaTreeVizPathVertex* debugDeltaPathTree(const App& app);
[[nodiscard]] int accumulationSampleIndex(const App& app);
[[nodiscard]] math::uint2 renderSize(const App& app);
[[nodiscard]] math::uint2 displaySize(const App& app);
[[nodiscard]] bool accumulationCompleted(const App& app);
[[nodiscard]] float avgTimePerFrame(const App& app);
[[nodiscard]] std::string resolutionInfo(const App& app);
[[nodiscard]] std::string fpsInfo(const App& app);

} // namespace caustica
