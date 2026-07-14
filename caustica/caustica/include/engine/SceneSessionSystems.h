#pragma once

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <render/RenderSessionState.h>
#include <render/RenderRuntimeState.h>
#include <render/SessionDiagnostics.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rhi/nvrhi.h>

class RenderTargets;
struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;

#include <scene/SceneManager.h>

namespace caustica
{

class App;
class GpuRenderSubsystem;
class Material;
struct MeshInfo;
class PlanarView;
class SceneViewState;

namespace render
{
class WorldRenderer;
}

namespace sceneSession
{

[[nodiscard]] GpuRenderSubsystem* gpuRender(const App& app);
[[nodiscard]] GpuDevice* gpuDevice(const App& app);
[[nodiscard]] SceneManager* sceneManager(const App& app);
[[nodiscard]] render::WorldRenderer* worldRenderer(const App& app);
[[nodiscard]] PathTracerSettings* settings(const App& app);
[[nodiscard]] render::RenderRuntimeState* runtimeState(const App& app);
[[nodiscard]] render::SessionDiagnostics* diagnostics(const App& app);
[[nodiscard]] const CommandLineOptions* cmdLine(const App& app);
[[nodiscard]] SceneViewState* viewState(const App& app);

[[nodiscard]] std::shared_ptr<Scene> scene(const App& app);
[[nodiscard]] const std::vector<std::string>& availableScenes(const App& app);
[[nodiscard]] std::string currentSceneName(const App& app);
[[nodiscard]] bool shouldSkipRender(const App& app);
[[nodiscard]] bool shouldRenderWhenUnfocused(const App& app);

void attachGpuRenderSubsystem(App& app, GpuRenderSubsystem& gpuRenderSubsystem);
void initStreamlineAndWindow(App& app);
void initializeSession(App& app, const std::string& preferredScene);

void beginFrameScheduled(App& app);
void animate(App& app, float elapsedTimeSeconds);
void updateCamera(App& app, float elapsedTimeSeconds);
void tickSimulationAndFrameTiming(App& app, float elapsedTimeSeconds);
void updateWindowTitle(App& app);
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);
void renderScene(App& app, GpuDevice& gpuDevice);
void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice);
void backBufferResizing(App& app);

void onSceneLoaded(App& app);
void onSceneUnloading(App& app);

void setCurrentScene(App& app, const std::string& sceneName, bool forceReload = false);
[[nodiscard]] bool isSceneLoading(const App& app);
[[nodiscard]] bool isSceneLoaded(const App& app);

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub = true);
[[nodiscard]] uint32_t gaussianSplatCount(const App& app);
[[nodiscard]] uint32_t gaussianSplatObjectCount(const App& app);
[[nodiscard]] const std::string& gaussianSplatFileName(const App& app);

void collectUncompressedTextures(App& app);
[[nodiscard]] bool hasAsyncLoadingInProgress(const App& app);

void saveCurrentCamera(const App& app);
void loadCurrentCamera(App& app);
[[nodiscard]] std::string currentCameraPosDirUp(const App& app);
bool setCurrentCameraPosDirUp(App& app, const std::string& val);
void setCameraVerticalFOV(App& app, float cameraFOV);
void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height);
void clearCameraIntrinsics(App& app);

[[nodiscard]] float cameraVerticalFOV(const App& app);
[[nodiscard]] const FirstPersonCamera& currentCamera(const App& app);
[[nodiscard]] const std::shared_ptr<PlanarView>& currentView(const App& app);
[[nodiscard]] const PlanarView& view(const App& app);
[[nodiscard]] uint sceneCameraCount(const App& app);
[[nodiscard]] uint& selectedCameraIndex(App& app);

void setSceneTime(App& app, double sceneTime);
[[nodiscard]] double sceneTime(const App& app);
[[nodiscard]] double& sceneTimeRef(App& app);

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride);
[[nodiscard]] const std::string& envMapLocalPath(const App& app);
[[nodiscard]] const std::string& envMapOverrideSource(const App& app);
[[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList(App& app);

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh);
void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation);
void debugDrawLine(App& app, math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);

[[nodiscard]] std::shared_ptr<Material> findMaterial(const App& app, int materialID);
[[nodiscard]] ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex);

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

void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work);

} // namespace sceneSession

} // namespace caustica
