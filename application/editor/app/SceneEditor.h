#pragma once

#include <engine/SceneSessionHooks.h>
#include <core/progress.h>

struct GLFWwindow;
#include <engine/SceneSessionSystems.h>
#include <engine/SceneViewState.h>
#include <engine/GpuRenderSubsystem.h>
#include <math/math.h>
#include <render/SceneLightingPasses.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>
#include <scene/SceneManager.h>

#include "ui/EditorUIData.h"
#include "EditorInputRouter.h"
#include "SceneContentEditor.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class App;
class Event;
class RootFileSystem;
} // namespace caustica

class RenderTargets;
class ZoomTool;
struct DebugFeedbackStruct;

#if CAUSTICA_WITH_PYTHON
class PythonScripting;
#endif

class GameScene;

namespace caustica::editor
{

class CaptureScriptManager;

using namespace caustica::math;

// Scene editor shell (mesh edit, Inspector, Capture). Frame logic lives in sceneSession systems.
class SceneEditor
{
public:
    SceneEditor(const CommandLineOptions& cmdLine,
        caustica::render::RenderSessionState& sessionState,
        EditorUIState& editorState,
        caustica::render::SessionDiagnostics& diagnostics);
    SceneEditor(const CommandLineOptions& cmdLine, EditorUIData& ui, caustica::render::SessionDiagnostics& diagnostics);

    ~SceneEditor();

    [[nodiscard]] SceneViewState& viewState() { return m_viewState; }
    [[nodiscard]] const SceneViewState& viewState() const { return m_viewState; }
    [[nodiscard]] SceneSessionHooks& hooks() { return m_hooks; }
    [[nodiscard]] const SceneSessionHooks& hooks() const { return m_hooks; }

    void setApp(App& app) { m_app = &app; }
    [[nodiscard]] App* app() const { return m_app; }

    [[nodiscard]] GpuRenderSubsystem* gpuRender() const;
    [[nodiscard]] GpuDevice& gpuDevice() const;
    [[nodiscard]] nvrhi::IDevice* device() const;
    [[nodiscard]] uint32_t frameIndex() const;

    [[nodiscard]] render::RenderSessionState& renderSessionState() { return m_sessionState; }
    [[nodiscard]] const render::RenderSessionState& renderSessionState() const { return m_sessionState; }
    [[nodiscard]] PathTracerSettings& pathTracerSettings() { return m_settings; }
    [[nodiscard]] const PathTracerSettings& pathTracerSettings() const { return m_settings; }
    [[nodiscard]] render::RenderRuntimeState& renderRuntimeState() { return m_renderState; }
    [[nodiscard]] const render::RenderRuntimeState& renderRuntimeState() const { return m_renderState; }
    [[nodiscard]] const CommandLineOptions& cmdLine() const { return m_cmdLine; }

    [[nodiscard]] std::shared_ptr<Scene> scene() const;
    [[nodiscard]] bool shouldSkipRender() const;

    SceneManager* sceneManager();
    caustica::render::WorldRenderer* worldRenderer();
    caustica::render::SceneLightingPasses& lightingPasses();
    const SceneManager* sceneManager() const;
    caustica::render::WorldRenderer* worldRenderer() const;
    const caustica::render::SceneLightingPasses& lightingPasses() const;

    const std::unique_ptr<::GameScene>& GetGame() const { return m_sampleGame; }

    EditorUIData& GetUIData() { assert(m_editorUi); return *m_editorUi; }
    const EditorUIData& GetUIData() const { assert(m_editorUi); return *m_editorUi; }
    EditorUIState& GetEditorUIState() { return m_editor; }
    const EditorUIState& GetEditorUIState() const { return m_editor; }

    void HandleDroppedFiles();
    bool LoadMeshFile(const std::filesystem::path& filePath);
    bool LoadGltfMeshFile(const std::filesystem::path& filePath);
    bool LoadObjMeshFile(const std::filesystem::path& filePath);
    void FinalizeRuntimeSceneMutation(caustica::ecs::Entity importedRoot);
    bool DeleteSceneNode(caustica::ecs::Entity entity);
    void RequestFullRebuild();
    std::vector<dm::float3> getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const;
    void setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    std::vector<dm::float3> getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh);
    std::vector<dm::float3> getMeshVerticesWorld(caustica::ecs::Entity entity);
    void setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    void setMeshVerticesWorld(caustica::ecs::Entity entity,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);

    void attachGpuRenderSubsystem(caustica::GpuRenderSubsystem& gpuRenderSubsystem);
    void initializeSession(const std::string& preferredScene);
    void initStreamlineAndWindow();

    void PrepareEditorFrame();
    void CaptureScriptPreRender();
    void CaptureScriptPostRender(std::function<bool(const char* fileName)> saveTexture);
    ::ZoomTool* GetOrCreateZoomTool();

    bool ShowDeltaTree() const;
    void ResolvePickFeedback(const DebugFeedbackStruct& feedback);
    bool ConsumeExperimentalPhotoScreenshot();

    void onEvent(caustica::Event& event);

    void setSceneTime(double sceneTime);
    double sceneTime() const;

    [[nodiscard]] std::shared_ptr<Material> findMaterial(int materialID) const;
    [[nodiscard]] ecs::Entity findEntityByInstanceIndex(int instanceIndex) const;
    [[nodiscard]] const FirstPersonCamera& currentCamera() const;
    [[nodiscard]] const std::shared_ptr<PlanarView>& currentView() const;
    [[nodiscard]] const DebugFeedbackStruct& feedbackData() const;
    [[nodiscard]] const DeltaTreeVizPathVertex* debugDeltaPathTree() const;
    [[nodiscard]] int accumulationSampleIndex() const;
    [[nodiscard]] math::uint2 renderSize() const;
    [[nodiscard]] math::uint2 displaySize() const;
    [[nodiscard]] std::string currentSceneName() const;
    [[nodiscard]] const std::vector<std::string>& availableScenes() const;
    void setCurrentScene(const std::string& sceneName, bool forceReload = false);

    bool loadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    [[nodiscard]] uint32_t gaussianSplatCount() const;
    [[nodiscard]] uint32_t gaussianSplatObjectCount() const;

    [[nodiscard]] uint sceneCameraCount() const;
    [[nodiscard]] uint& selectedCameraIndex();

    void saveCurrentCamera() const;
    void loadCurrentCamera();
    [[nodiscard]] std::string currentCameraPosDirUp() const;
    bool setCurrentCameraPosDirUp(const std::string& val);
    void setCameraVerticalFOV(float cameraFOV);
    [[nodiscard]] float cameraVerticalFOV() const;

    [[nodiscard]] std::string resolutionInfo() const;
    [[nodiscard]] std::string fpsInfo() const;
    auto& uncompressedTextures() { return m_viewState.uncompressedTextures; }

    [[nodiscard]] const std::string& envMapLocalPath() const;
    [[nodiscard]] const std::string& envMapOverrideSource() const;
    [[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList();
    void setEnvMapOverrideSource(const std::string& envMapOverride);

    [[nodiscard]] ProgressBar& loadingProgress() { return m_viewState.progressLoading; }
    [[nodiscard]] bool hasAsyncLoadingInProgress() const;
    [[nodiscard]] bool accumulationCompleted() const;
    [[nodiscard]] GLFWwindow* glfwWindow() const;

    [[nodiscard]] float avgTimePerFrame() const;
    void debugDrawLine(math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);

    const std::unique_ptr<::ZoomTool>& GetZoomTool() const { return m_zoomTool; }
    const std::unique_ptr<CaptureScriptManager>& GetCaptureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting>& GetPythonScripting() const { return m_pythonScripting; }
#endif

private:
    void bindHooks();
    void onBeginFrameScheduled();
    void onBeforeInitialSceneLoad();
    void onAnimateBegin(float& elapsedTimeSeconds);
    void onAnimateGameTick(float elapsedTimeSeconds, bool enableAnimations);
    void onAnimateUpdateSceneTime(float elapsedTimeSeconds, bool enableAnimations, bool enableAnimationUpdate);
    void onAnimateGameCamera(float elapsedTimeSeconds);
    void onAnimateEnd(float elapsedTimeSeconds);
    void onSceneUnloading();
    void onSceneLoaded();
    void onSceneLoadedEarly();
    void onSceneLoadedBeforeGpuPrep();
    void onSceneLoadedAfterCollectTextures();
    void onSceneLoadedComplete();
    void updateWindowTitle();
    void afterWorldRender(caustica::GpuDevice& gpuDevice);
    bool shouldRenderWhenUnfocused() const;

    const CommandLineOptions& m_cmdLine;
    render::RenderSessionState& m_sessionState;
    PathTracerSettings& m_settings;
    render::RenderRuntimeState& m_renderState;
    render::SessionDiagnostics& m_sessionDiagnostics;

    SceneViewState m_viewState;
    SceneSessionHooks m_hooks;

    App* m_app = nullptr;
    GpuRenderSubsystem* m_gpuRenderSubsystem = nullptr;

    EditorUIState& m_editor;
    EditorUIData* m_editorUi = nullptr;

    std::unique_ptr<CaptureScriptManager> m_captureScriptManager;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting> m_pythonScripting;
#endif

    EditorInputRouter m_inputRouter;
    SceneContentEditor m_contentEditor;

    std::shared_ptr<caustica::RootFileSystem> m_RootFS;

    std::unique_ptr<::GameScene> m_sampleGame;

    std::unique_ptr<::ZoomTool> m_zoomTool;
};

} // namespace caustica::editor
