#pragma once

#include <core/progress.h>

#include <engine/SceneViewState.h>
#include <math/math.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>

#include "ui/EditorUIData.h"
#include "EditorInputRouter.h"
#include "EditorResources.h"
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
} // namespace caustica

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

// Editor shell: selection/UI/capture/game/content. Scene/render queries go through
// App + EditorAccess helpers (sessionCamera / pathTracing / gpuSharedCaches), not this class.
class SceneEditor
{
public:
    SceneEditor(const CommandLineOptions& cmdLine,
        caustica::render::RenderAppState& renderAppState,
        EditorUIState& editorState,
        caustica::render::AppDiagnostics& diagnostics);
    SceneEditor(const CommandLineOptions& cmdLine, EditorUIData& ui, caustica::render::AppDiagnostics& diagnostics);

    ~SceneEditor();

    [[nodiscard]] SceneViewState& viewState() { return m_viewState; }
    [[nodiscard]] const SceneViewState& viewState() const { return m_viewState; }

    void setApp(App& app) { m_app = &app; }
    [[nodiscard]] App* app() const { return m_app; }

    [[nodiscard]] render::RenderAppState& renderAppState() { return m_renderAppState; }
    [[nodiscard]] const render::RenderAppState& renderAppState() const { return m_renderAppState; }
    [[nodiscard]] PathTracerSettings& pathTracerSettings() { return m_settings; }
    [[nodiscard]] const PathTracerSettings& pathTracerSettings() const { return m_settings; }
    [[nodiscard]] render::RenderRuntimeState& renderRuntimeState() { return m_renderState; }
    [[nodiscard]] const render::RenderRuntimeState& renderRuntimeState() const { return m_renderState; }
    [[nodiscard]] const CommandLineOptions& cmdLine() const { return m_cmdLine; }

    const std::unique_ptr<::GameScene>& game() const { return m_sampleGame; }

    EditorUIData& uiData() { assert(m_editorUi); return *m_editorUi; }
    const EditorUIData& uiData() const { assert(m_editorUi); return *m_editorUi; }
    EditorUIState& editorUIState() { return m_editor; }
    const EditorUIState& editorUIState() const { return m_editor; }
    [[nodiscard]] bool hasEditorUiData() const { return m_editorUi != nullptr; }
    [[nodiscard]] EditorState& editorState() { return m_editorState; }
    [[nodiscard]] const EditorState& editorState() const { return m_editorState; }
    [[nodiscard]] CaptureScriptState& captureScriptState() { return m_captureScriptState; }
    [[nodiscard]] const CaptureScriptState& captureScriptState() const { return m_captureScriptState; }
    [[nodiscard]] SelectionState& selectionState() { return m_selectionState; }
    [[nodiscard]] const SelectionState& selectionState() const { return m_selectionState; }
    [[nodiscard]] EditorCameraState& editorCameraState() { return m_editorCameraState; }
    [[nodiscard]] const EditorCameraState& editorCameraState() const { return m_editorCameraState; }

    void handleDroppedFiles();
    bool loadMeshFile(const std::filesystem::path& filePath);
    bool loadGltfMeshFile(const std::filesystem::path& filePath);
    bool loadObjMeshFile(const std::filesystem::path& filePath);
    bool deleteSceneNode(caustica::ecs::Entity entity);
    void processPendingSceneDeletes();
    void requestFullRebuild();
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

    void bindSessionCameraSideEffects();

    void onBeforeInitialSceneLoad();
    void onAnimateBegin(float& elapsedTimeSeconds);
    void onAnimateGameTick(float elapsedTimeSeconds, bool enableAnimations);
    void onAnimateUpdateSceneTime(float elapsedTimeSeconds, bool enableAnimations, bool enableAnimationUpdate);
    void onAnimateGameCamera(float elapsedTimeSeconds);
    void onAnimateEnd(float elapsedTimeSeconds);
    void onSceneUnloading();
    void onSceneLoadedFromLoader();
    void syncLoadedSceneSystems();
    void updateWindowTitle();
    void afterWorldRender(caustica::GpuDevice& gpuDevice);

    void prepareEditorFrame();
    void captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture);
    ::ZoomTool* getOrCreateZoomTool();

    bool showDeltaTree() const;
    void resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick);
    [[nodiscard]] ecs::Entity pickGaussianSplatAtPixel(math::uint2 renderPixel) const;
    bool consumeExperimentalPhotoScreenshot();

    void onEvent(caustica::Event& event);

    void setSceneTime(double sceneTime);
    double sceneTime() const;

    auto& uncompressedTextures() { return m_viewState.uncompressedTextures; }
    [[nodiscard]] ProgressBar& loadingProgress() { return m_viewState.progressLoading; }

    const std::unique_ptr<::ZoomTool>& zoomTool() const { return m_zoomTool; }
    const std::unique_ptr<CaptureScriptManager>& captureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting>& pythonScripting() const { return m_pythonScripting; }
#endif

private:
    void onSceneLoaded();
    void onSceneLoadedEarly();
    void onSceneLoadedBeforeGpuPrep();
    void onSceneLoadedAfterCollectTextures();
    void onSceneLoadedComplete();

    const CommandLineOptions& m_cmdLine;
    render::RenderAppState& m_renderAppState;
    PathTracerSettings& m_settings;
    render::RenderRuntimeState& m_renderState;
    render::AppDiagnostics& m_diagnostics;

    SceneViewState m_viewState;
    EditorState m_editorState;

    App* m_app = nullptr;

    EditorUIState& m_editor;
    EditorUIData* m_editorUi = nullptr;
    SelectionState m_selectionState;
    EditorCameraState m_editorCameraState;

    std::unique_ptr<CaptureScriptManager> m_captureScriptManager;
    CaptureScriptState m_captureScriptState;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting> m_pythonScripting;
#endif

    EditorInputRouter m_inputRouter;
    SceneContentEditor m_contentEditor;

    std::unique_ptr<::GameScene> m_sampleGame;

    std::unique_ptr<::ZoomTool> m_zoomTool;
};

} // namespace caustica::editor
