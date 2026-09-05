#pragma once

#include <core/command_line.h>
#include <core/progress.h>

#include "EditorCommandLine.h"

#include <engine/SceneViewState.h>
#include <math/math.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>

#include "ui/EditorUIData.h"
#include "EditorInputRouter.h"
#include "EditorResources.h"
#include "EditorUndoStack.h"
#include "EditorUndoCommands.h"
#include "SceneContentEditor.h"

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class App;
class EngineApp;
class Event;
class GpuDevice;
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
class RenderSettingsConsoleBinding;

using namespace caustica::math;

// Editor application shell: owns cmdline / UI / diagnostics / console, plus
// selection/capture/game/content. Scene/render queries go through App + EditorAccess.
class SceneEditor
{
public:
    SceneEditor();
    ~SceneEditor();

    void bindEngine(EngineApp& engine);

    [[nodiscard]] SceneViewState& viewState();
    [[nodiscard]] const SceneViewState& viewState() const;

    void setApp(App& app) { m_app = &app; }
    [[nodiscard]] App* app() const { return m_app; }

    [[nodiscard]] render::RenderAppState& renderAppState();
    [[nodiscard]] const render::RenderAppState& renderAppState() const;
    [[nodiscard]] PathTracerSettings& pathTracerSettings();
    [[nodiscard]] const PathTracerSettings& pathTracerSettings() const;
    [[nodiscard]] render::RenderRuntimeState& renderRuntimeState();
    [[nodiscard]] const render::RenderRuntimeState& renderRuntimeState() const;
    [[nodiscard]] CommandLineOptions& cmdLine();
    [[nodiscard]] const CommandLineOptions& cmdLine() const;
    [[nodiscard]] EditorCommandLine& editorCmdLine() { return m_editorCmdLine; }
    [[nodiscard]] const EditorCommandLine& editorCmdLine() const { return m_editorCmdLine; }
    [[nodiscard]] render::AppDiagnostics& diagnostics();
    [[nodiscard]] const render::AppDiagnostics& diagnostics() const;

    [[nodiscard]] RenderSettingsConsoleBinding* console() const { return m_console.get(); }
    void setConsole(std::unique_ptr<RenderSettingsConsoleBinding> console);

    const std::unique_ptr<::GameScene>& game() const { return m_game; }

    EditorUIData& uiData() { return *m_editorUiData; }
    const EditorUIData& uiData() const { return *m_editorUiData; }
    EditorUIState& editorUIState() { return m_editorUiData->editor; }
    const EditorUIState& editorUIState() const { return m_editorUiData->editor; }
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
    bool deleteEntity(caustica::ecs::Entity entity);
    void processPendingSceneDeletes();
    void processPendingSceneCreates();
    [[nodiscard]] caustica::ecs::Entity createBuiltinMesh(BuiltinPrimitiveKind kind);
    [[nodiscard]] caustica::ecs::Entity createLight(EditorLightKind kind);
    void requestCreateBuiltinMesh(BuiltinPrimitiveKind kind);
    void requestCreateLight(EditorLightKind kind);
    void requestFullRebuild();
    std::vector<math::float3> getMeshVertices(caustica::ecs::Entity entity) const;
    void setMeshVertices(caustica::ecs::Entity entity,
        const std::vector<math::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    std::vector<math::float3> getMeshVerticesWorld(caustica::ecs::Entity entity);
    void setMeshVerticesWorld(caustica::ecs::Entity entity,
        const std::vector<math::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);

    void bindCameraControllerSideEffects();

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

    // File menu: open/save scene JSON (Save patches transforms into cached document).
    bool openSceneFromDialog();
    bool saveScene();
    bool saveSceneAsFromDialog();
    [[nodiscard]] bool canSaveScene() const;

    void prepareEditorFrame();
    void captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture);
    ::ZoomTool* getOrCreateZoomTool();

    bool showDeltaTree() const;
    void resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick);
    [[nodiscard]] ecs::Entity pickGaussianSplatAtPixel(math::uint2 displayPixel) const;
    bool consumeExperimentalPhotoScreenshot();

    void onEvent(caustica::Event& event);

    [[nodiscard]] EditorUndoStack& undoStack() { return m_undoStack; }
    [[nodiscard]] const EditorUndoStack& undoStack() const { return m_undoStack; }

    // Queue undo/redo / file dialogs so they run after EditorUI/gizmo commit
    // (same-frame safe; FileDialog must not block inside ImGui buildUI).
    void requestUndo();
    void requestRedo();
    void requestOpenSceneFromDialog();
    void requestSaveScene();
    void requestSaveSceneAsFromDialog();
    void processPendingEditActions();

    bool undo();
    bool redo();
    void commitTransformEdit(
        ecs::Entity entity,
        const LocalTransformSnapshot& before,
        const LocalTransformSnapshot& after);

    void setSceneTime(double sceneTime);
    double sceneTime() const;
    void setTimelineTime(double timelineTime);
    double timelineTime() const;
    bool insertTransformKeyframe(ecs::Entity entity, float timeSeconds);
    bool deleteTransformKeyframe(ecs::Entity entity, float timeSeconds);
    [[nodiscard]] bool hasTransformKeyframe(ecs::Entity entity, float timeSeconds) const;
    bool insertVisibilityKeyframe(ecs::Entity entity, float timeSeconds);
    bool deleteVisibilityKeyframe(ecs::Entity entity, float timeSeconds);
    [[nodiscard]] bool hasVisibilityKeyframe(ecs::Entity entity, float timeSeconds) const;
    [[nodiscard]] bool canAnimateVisibility(ecs::Entity entity) const;
    [[nodiscard]] std::vector<float> keyframeTimes(ecs::Entity entity = ecs::NullEntity) const;
    [[nodiscard]] std::vector<float> visibilityKeyframeTimes(ecs::Entity entity = ecs::NullEntity) const;
    [[nodiscard]] float animationDuration() const;

    // DiscontinuousSeek: click/jump — zero MVs and reset TAA/NRD/DLSS history.
    // ContinuousScrub: playhead drag / ±1 step — same as playback (keep temporal history).
    enum class AnimationEvaluateMode
    {
        DiscontinuousSeek,
        ContinuousScrub,
    };
    void evaluateAnimationsAt(
        float timeSeconds,
        AnimationEvaluateMode mode = AnimationEvaluateMode::DiscontinuousSeek);

    auto& uncompressedTextures() { return viewState().uncompressedTextures; }
    [[nodiscard]] ProgressBar& loadingProgress() { return viewState().progressLoading; }

    const std::unique_ptr<::ZoomTool>& zoomTool() const { return m_zoomTool; }
    const std::unique_ptr<CaptureScriptManager>& captureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting>& pythonScripting() const { return m_pythonScripting; }
#endif

private:
    void consumeCompletedMaterialPickFeedback();
    void consumeCompletedInstancePickFeedback();
    void onSceneLoadedEarly();
    void onSceneLoadedBeforeGpuPrep();
    void onSceneLoadedAfterCollectTextures();
    void onSceneLoadedComplete();

    EditorCommandLine m_editorCmdLine;
    std::unique_ptr<EditorUIData> m_editorUiData;
    std::unique_ptr<RenderSettingsConsoleBinding> m_console;

    EngineApp* m_engine = nullptr;
    App* m_app = nullptr;

    EditorState m_editorState;
    SelectionState m_selectionState;
    EditorCameraState m_editorCameraState;

    std::unique_ptr<CaptureScriptManager> m_captureScriptManager;
    CaptureScriptState m_captureScriptState;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting> m_pythonScripting;
#endif

    EditorInputRouter m_inputRouter;
    SceneContentEditor m_contentEditor;
    EditorUndoStack m_undoStack;
    enum class PendingEditAction : uint8_t
    {
        None,
        Undo,
        Redo,
        OpenScene,
        SaveScene,
        SaveSceneAs,
    };
    PendingEditAction m_pendingEditAction = PendingEditAction::None;
    ecs::Entity m_editorAnimationEntity = ecs::NullEntity;

    std::unique_ptr<::GameScene> m_game;

    std::unique_ptr<::ZoomTool> m_zoomTool;

    // Render-to-logic mailbox for material feedback. The material editor and
    // RenderRuntimeState are logic-thread owned; the render callback only
    // publishes POD results here.
    std::atomic<int> m_completedMaterialPickGpuId{-1};
    std::atomic<uint64_t> m_completedMaterialPickRequestId{0};
    uint64_t m_consumedMaterialPickRequestId = 0;
    std::atomic<int> m_completedInstancePickIndex{-1};
    std::atomic<uint64_t> m_completedInstancePickPosition{0};
    std::atomic<uint64_t> m_completedInstancePickRequestId{0};
    uint64_t m_consumedInstancePickRequestId = 0;
};

} // namespace caustica::editor
