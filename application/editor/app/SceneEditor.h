#pragma once

#include <cassert>

#include <engine/SceneRuntime.h>
#include <math/math.h>

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

// Scene editor shell (mesh edit, Inspector, Capture). Path tracer is owned by GpuRenderSubsystem.
class SceneEditor : public caustica::SceneRuntime
{
public:
    SceneEditor(const CommandLineOptions& cmdLine,
        caustica::render::RenderSessionState& sessionState,
        EditorUIState& editorState,
        caustica::render::SessionDiagnostics& diagnostics);
    SceneEditor(const CommandLineOptions& cmdLine, EditorUIData& ui, caustica::render::SessionDiagnostics& diagnostics);

    ~SceneEditor() override;

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
    std::vector<dm::float3> GetMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const;
    void SetMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    std::vector<dm::float3> GetMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh);
    std::vector<dm::float3> GetMeshVerticesWorld(caustica::ecs::Entity entity);
    void SetMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    void SetMeshVerticesWorld(caustica::ecs::Entity entity,
        const std::vector<dm::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);

    void bindGpuRenderSubsystem(caustica::GpuRenderSubsystem& gpuRenderSubsystem) override;
    void Init(const std::string& preferredScene,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void initStreamlineAndWindow();

    void PrepareEditorFrame();
    void CaptureScriptPreRender();
    void CaptureScriptPostRender(std::function<bool(const char* fileName)> saveTexture);
    ::ZoomTool* GetOrCreateZoomTool();

    void SceneUnloading() override;
    void SceneLoaded() override;
    bool ShouldRenderUnfocused() const override;
    void Animate(float elapsedTimeSeconds) override;
    void afterWorldRender(caustica::GpuDevice& gpuDevice) override;

    bool ShowDeltaTree() const;
    void ResolvePickFeedback(const DebugFeedbackStruct& feedback);
    bool ConsumeExperimentalPhotoScreenshot();

    void onEvent(caustica::Event& event);

    void SetSceneTime(double sceneTime);
    double GetSceneTime();

    const std::unique_ptr<::ZoomTool>& GetZoomTool() const { return m_zoomTool; }

    const std::unique_ptr<CaptureScriptManager>& GetCaptureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting>& GetPythonScripting() const { return m_pythonScripting; }
#endif

protected:
    void onBeforeInitialSceneLoad() override;
    void onAnimateBegin(float& elapsedTimeSeconds) override;
    void onAnimateGameTick(float elapsedTimeSeconds, bool enableAnimations) override;
    void onAnimateUpdateSceneTime(float elapsedTimeSeconds, bool enableAnimations, bool enableAnimationUpdate) override;
    void onAnimateGameCamera(float elapsedTimeSeconds) override;
    void onAnimateEnd(float elapsedTimeSeconds) override;
    void onSceneLoadedEarly() override;
    void onSceneLoadedBeforeGpuPrep() override;
    void onSceneLoadedAfterCollectTextures() override;
    void onSceneLoadedComplete() override;
    void updateWindowTitle() override;

private:
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
