#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"

#include <render/WorldRenderer/WorldRenderer.h>
#include <render/Core/TextureUtils.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/GpuRenderSubsystem.h>
#include <core/path_utils.h>
#include <EditorUI.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include "game/GameScene.h"

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

namespace caustica::editor
{

SceneEditor::SceneEditor(const CommandLineOptions& cmdLine,
    caustica::render::RenderSessionState& sessionState,
    EditorUIState& editorState,
    caustica::render::SessionDiagnostics& diagnostics)
    : SceneRuntime(cmdLine, sessionState, diagnostics)
    , m_editor(editorState)
    , m_inputRouter()
    , m_contentEditor(*this)
{
    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_sessionState, m_cmdLine);
}

SceneEditor::SceneEditor(const CommandLineOptions& cmdLine,
    EditorUIData& ui,
    caustica::render::SessionDiagnostics& diagnostics)
    : SceneEditor(cmdLine, ui.session, ui.editor, diagnostics)
{
    m_editorUi = &ui;
}

SceneEditor::~SceneEditor()
{
#if CAUSTICA_WITH_PYTHON
    m_pythonScripting.reset();
#endif
}

void SceneEditor::initStreamlineAndWindow()
{
    SceneRuntime::initStreamlineAndWindow();
#if CAUSTICA_WITH_PYTHON
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

void SceneEditor::bindGpuRenderSubsystem(caustica::GpuRenderSubsystem& gpuRenderSubsystem)
{
    SceneRuntime::bindGpuRenderSubsystem(gpuRenderSubsystem);
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_progressLoading.Set(95);
}

void SceneEditor::Init(const std::string& preferredScene,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    SceneRuntime::Init(preferredScene, shaderFactory);
}

void SceneEditor::PrepareEditorFrame()
{
    HandleDroppedFiles();
    m_settings.DebugExploreDeltaTree = m_editor.ShowDeltaTree;
}

void SceneEditor::SceneUnloading()
{
    m_editor.TogglableNodes = nullptr;
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

    if (m_sampleGame != nullptr)
        m_sampleGame->SceneUnloading();

    SceneRuntime::SceneUnloading();
}

void SceneEditor::onSceneLoadedEarly()
{
    if (m_sampleGame != nullptr)
    {
        const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
        m_sampleGame->SceneLoaded(
            GetSceneManager()->getScene(),
            GetSceneManager()->getCurrentScenePath(),
            assetsRoot);
    }
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (m_editor.TogglableNodes == nullptr)
    {
        auto scene = GetSceneManager()->getScene();
        auto* ew = scene ? scene->GetEntityWorld() : nullptr;
        if (ew)
        {
            m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
            UpdateTogglableNodes(*m_editor.TogglableNodes, *ew, ew->root());
        }
    }
}

void SceneEditor::onSceneLoadedAfterCollectTextures()
{
    LocalConfig::PostSceneLoad(*this, m_sessionState, m_editor);
}

void SceneEditor::onSceneLoadedComplete()
{
#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting
        && (!m_cmdLine.pythonScript.empty() || !m_cmdLine.pythonExpr.empty()))
    {
        if (m_pythonScripting->Initialize())
        {
            if (!m_cmdLine.pythonScript.empty())
                m_pythonScripting->QueueScriptFile(m_cmdLine.pythonScript);
            if (!m_cmdLine.pythonExpr.empty())
                m_pythonScripting->QueueScriptString(m_cmdLine.pythonExpr, "<--pythonExpr>");
        }
    }
#endif
}

void SceneEditor::SceneLoaded()
{
    SceneRuntime::SceneLoaded();
}

void SceneEditor::onAnimateBegin(float& fElapsedTimeSeconds)
{
    m_captureScriptManager->PreAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting && IsSceneLoaded())
        m_pythonScripting->ProcessPendingScripts();
#endif
}

void SceneEditor::onAnimateGameTick(float fElapsedTimeSeconds, bool enableAnimations)
{
    if (m_sampleGame)
        m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);
}

void SceneEditor::onAnimateUpdateSceneTime(float /*fElapsedTimeSeconds*/, bool enableAnimations, bool /*enableAnimationUpdate*/)
{
    if (enableAnimations && m_sampleGame && m_sampleGame->IsInitialized())
        m_sceneTime = m_sampleGame->GetGameTime();
}

void SceneEditor::onAnimateGameCamera(float fElapsedTimeSeconds)
{
    if (m_sampleGame)
        m_sampleGame->TickCamera(fElapsedTimeSeconds, GetCameraController()->camera());
}

void SceneEditor::onAnimateEnd(float /*fElapsedTimeSeconds*/)
{
    m_captureScriptManager->PostAnim();
}

void SceneEditor::Animate(float fElapsedTimeSeconds)
{
    SceneRuntime::Animate(fElapsedTimeSeconds);
}

void SceneEditor::updateWindowTitle()
{
    SceneRuntime::updateWindowTitle();
}

bool SceneEditor::ShouldRenderUnfocused() const
{
    if (GetWorldRenderer()->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork())
        return true;

    if (m_editor.RenderWhenOutOfFocus)
        return true;

    return SceneRuntime::ShouldRenderUnfocused();
}

void SceneEditor::SetSceneTime(double sceneTime)
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_sceneTime = sceneTime;
}

double SceneEditor::GetSceneTime()
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        return m_sampleGame->GetGameTime();
    return m_sceneTime;
}

void SceneEditor::HandleDroppedFiles()
{
    m_contentEditor.handleDroppedFiles(m_editor.PendingDroppedFiles);
}

bool SceneEditor::LoadMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadMeshFile(filePath);
}

bool SceneEditor::LoadGltfMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadGltfMeshFile(filePath);
}

bool SceneEditor::LoadObjMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadObjMeshFile(filePath);
}

void SceneEditor::FinalizeRuntimeSceneMutation(caustica::ecs::Entity importedRoot)
{
    m_contentEditor.finalizeRuntimeSceneMutation(importedRoot);
}

bool SceneEditor::DeleteSceneNode(caustica::ecs::Entity entity)
{
    return m_contentEditor.deleteSceneNode(entity);
}

void SceneEditor::RequestFullRebuild()
{
    m_contentEditor.requestFullRebuild();
}

std::vector<float3> SceneEditor::GetMeshVertices(const std::shared_ptr<MeshInfo>& mesh) const
{
    return m_contentEditor.getMeshVertices(mesh);
}

std::vector<float3> SceneEditor::GetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh)
{
    return m_contentEditor.getMeshVerticesWorld(mesh);
}

std::vector<float3> SceneEditor::GetMeshVerticesWorld(caustica::ecs::Entity entity)
{
    return m_contentEditor.getMeshVerticesWorld(entity);
}

void SceneEditor::SetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVerticesWorld(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void SceneEditor::SetMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVerticesWorld(entity, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void SceneEditor::SetMeshVertices(const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVertices(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
}

ZoomTool* SceneEditor::GetOrCreateZoomTool()
{
    if (m_zoomTool == nullptr)
        m_zoomTool = std::make_unique<ZoomTool>(GetDevice(), GetShaderFactory());
    return m_zoomTool.get();
}

bool SceneEditor::ShowDeltaTree() const
{
    return m_editor.ShowDeltaTree;
}

void SceneEditor::ResolvePickFeedback(const DebugFeedbackStruct& feedback)
{
    if (m_renderState.Picking.MaterialRequested)
        m_editor.SelectedMaterial = FindMaterial(int(feedback.pickedMaterialID));
    if (m_renderState.Picking.InstanceRequested)
    {
        m_editor.SelectedEntity = FindEntityByInstanceIndex(int(feedback.pickedInstanceIndex));
        if (m_editor.SelectedEntity != caustica::ecs::NullEntity)
            m_editor.SelectedGaussianSplat = false;
    }
}

void SceneEditor::afterWorldRender(caustica::GpuDevice& gpuDevice)
{
    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
        return;

    if (m_settings.ContinuousDebugFeedback || m_renderState.Picking.hasActivePickRequest())
        ResolvePickFeedback(worldRenderer->getFeedbackData());

    SceneRuntime::afterWorldRender(gpuDevice);

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (!framebuffer)
            return false;
        nvrhi::ITexture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        auto* renderDevice = GetRenderDevice();
        if (!renderDevice)
            return false;
        return SaveTextureToFile(
            gpuDevice.GetDevice(), *renderDevice, texture, nvrhi::ResourceStates::Common, fileName);
    };
    CaptureScriptPostRender(saveFramebuffer);

    if (ConsumeExperimentalPhotoScreenshot())
    {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (framebuffer)
            worldRenderer->denoisedScreenshot(framebuffer->getDesc().colorAttachments[0].texture);
    }
}

bool SceneEditor::ConsumeExperimentalPhotoScreenshot()
{
    if (!m_editor.ExperimentalPhotoModeScreenshot)
        return false;
    m_editor.ExperimentalPhotoModeScreenshot = false;
    return true;
}

void SceneEditor::CaptureScriptPreRender()
{
    if (m_captureScriptManager)
        m_captureScriptManager->PreRender();
}

void SceneEditor::CaptureScriptPostRender(std::function<bool(const char* fileName)> saveTexture)
{
    if (m_captureScriptManager)
        m_captureScriptManager->PostRender(saveTexture);
}

void SceneEditor::onEvent(caustica::Event& event)
{
    m_inputRouter.onEvent(event);
}

} // namespace caustica::editor
