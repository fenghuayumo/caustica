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
class MeshInfo;
class PlanarView;
class SceneViewState;
struct SceneSessionHooks;

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
[[nodiscard]] SceneSessionHooks* hooks(const App& app);

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
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);
void renderScene(App& app, GpuDevice& gpuDevice);
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

// Thin query/mutation surface for Python and legacy call sites (delegates to sceneSession::).
class PathTracerSceneHost
{
public:
    explicit PathTracerSceneHost(App& app) : m_app(app) {}

    [[nodiscard]] App& app() { return m_app; }
    [[nodiscard]] const App& app() const { return m_app; }

    [[nodiscard]] GpuRenderSubsystem* gpuRender() const { return sceneSession::gpuRender(m_app); }
    [[nodiscard]] GpuDevice& gpuDevice() const;
    [[nodiscard]] nvrhi::IDevice* device() const;
    [[nodiscard]] uint32_t frameIndex() const;

    [[nodiscard]] render::RenderSessionState& renderSessionState() const;
    [[nodiscard]] PathTracerSettings& pathTracerSettings() const;
    [[nodiscard]] render::RenderRuntimeState& renderRuntimeState() const;

    [[nodiscard]] std::shared_ptr<Scene> scene() const { return sceneSession::scene(m_app); }
    [[nodiscard]] const std::vector<std::string>& availableScenes() const { return sceneSession::availableScenes(m_app); }
    [[nodiscard]] std::string currentSceneName() const { return sceneSession::currentSceneName(m_app); }

    void setCurrentScene(const std::string& sceneName, bool forceReload = false)
    {
        sceneSession::setCurrentScene(m_app, sceneName, forceReload);
    }

    [[nodiscard]] bool shouldSkipRender() const { return sceneSession::shouldSkipRender(m_app); }
    [[nodiscard]] bool isSceneLoading() const { return sceneSession::isSceneLoading(m_app); }
    [[nodiscard]] bool isSceneLoaded() const { return sceneSession::isSceneLoaded(m_app); }
    void requestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
    {
        sceneSession::requestMeshAccelRebuild(m_app, mesh);
    }

    [[nodiscard]] bool accumulationCompleted() const { return sceneSession::accumulationCompleted(m_app); }
    [[nodiscard]] nvrhi::ITexture* ldrColorTexture() const { return sceneSession::ldrColorTexture(m_app); }

    bool loadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub = true)
    {
        return sceneSession::loadGaussianSplatFile(m_app, fileName, convertRdfToRub);
    }

    [[nodiscard]] uint32_t gaussianSplatCount() const { return sceneSession::gaussianSplatCount(m_app); }
    [[nodiscard]] uint32_t gaussianSplatObjectCount() const { return sceneSession::gaussianSplatObjectCount(m_app); }
    [[nodiscard]] const std::string& gaussianSplatFileName() const { return sceneSession::gaussianSplatFileName(m_app); }

    void saveCurrentCamera() const { sceneSession::saveCurrentCamera(m_app); }
    void loadCurrentCamera() { sceneSession::loadCurrentCamera(m_app); }
    [[nodiscard]] std::string currentCameraPosDirUp() const { return sceneSession::currentCameraPosDirUp(m_app); }
    bool setCurrentCameraPosDirUp(const std::string& val) { return sceneSession::setCurrentCameraPosDirUp(m_app, val); }
    void setCameraVerticalFOV(float cameraFOV) { sceneSession::setCameraVerticalFOV(m_app, cameraFOV); }
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
    {
        sceneSession::setCameraIntrinsics(m_app, fx, fy, cx, cy, width, height);
    }
    void clearCameraIntrinsics() { sceneSession::clearCameraIntrinsics(m_app); }
    [[nodiscard]] float cameraVerticalFOV() const { return sceneSession::cameraVerticalFOV(m_app); }

    void setEnvMapOverrideSource(const std::string& envMapOverride)
    {
        sceneSession::setEnvMapOverrideSource(m_app, envMapOverride);
    }
    [[nodiscard]] const std::string& envMapLocalPath() const { return sceneSession::envMapLocalPath(m_app); }
    [[nodiscard]] const std::string& envMapOverrideSource() const { return sceneSession::envMapOverrideSource(m_app); }
    [[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList()
    {
        return sceneSession::envMapMediaList(m_app);
    }

    void setSceneTime(double t) { sceneSession::setSceneTime(m_app, t); }
    [[nodiscard]] double sceneTime() const { return sceneSession::sceneTime(m_app); }

    [[nodiscard]] std::shared_ptr<Material> findMaterial(int materialID) const
    {
        return sceneSession::findMaterial(m_app, materialID);
    }

    [[nodiscard]] const DebugFeedbackStruct& feedbackData() const { return sceneSession::feedbackData(m_app); }
    [[nodiscard]] int accumulationSampleIndex() const { return sceneSession::accumulationSampleIndex(m_app); }
    [[nodiscard]] math::uint2 renderSize() const { return sceneSession::renderSize(m_app); }
    [[nodiscard]] math::uint2 displaySize() const { return sceneSession::displaySize(m_app); }
    [[nodiscard]] float avgTimePerFrame() const { return sceneSession::avgTimePerFrame(m_app); }

private:
    App& m_app;
};

} // namespace caustica
