#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"

#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/TextureUtils.h>
#include <render/passes/debug/ZoomTool.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <core/path_utils.h>
#include <EditorUI.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include <GLFW/glfw3.h>

#include "game/GameScene.h"

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

extern const char* g_windowTitle;

namespace caustica::editor
{

SceneEditor::SceneEditor(const CommandLineOptions& cmdLine,
    caustica::render::RenderSessionState& sessionState,
    EditorUIState& editorState,
    caustica::render::SessionDiagnostics& diagnostics)
    : m_cmdLine(cmdLine)
    , m_sessionState(sessionState)
    , m_settings(sessionState.settings)
    , m_renderState(sessionState.runtime)
    , m_sessionDiagnostics(diagnostics)
    , m_editor(editorState)
    , m_selectionState(editorState)
    , m_editorCameraState(m_viewState)
    , m_inputRouter()
    , m_contentEditor(*this)
{
    m_viewState.progressLoading.Start("Initializing...");
    m_viewState.progressLoading.Set(50);
    m_inputRouter.bind(*this);
    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_sessionState, m_cmdLine);
    m_captureScriptState.manager = m_captureScriptManager.get();
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

GpuRenderSubsystem* SceneEditor::gpuRender() const
{
    if (m_gpuRenderSubsystem)
        return m_gpuRenderSubsystem;
    return m_app ? m_app->getSubsystem<GpuRenderSubsystem>() : nullptr;
}

GpuDevice& SceneEditor::gpuDevice() const
{
    assert(m_app && m_app->getGpuDevice());
    return *m_app->getGpuDevice();
}

nvrhi::IDevice* SceneEditor::device() const
{
    return gpuDevice().GetDevice();
}

uint32_t SceneEditor::frameIndex() const
{
    return gpuDevice().GetFrameIndex();
}

std::shared_ptr<Scene> SceneEditor::scene() const
{
    return m_app ? sceneSession::scene(*m_app) : nullptr;
}

bool SceneEditor::shouldSkipRender() const
{
    return m_app ? sceneSession::shouldSkipRender(*m_app) : true;
}

SceneManager* SceneEditor::sceneManager() { return gpuRender() ? gpuRender()->sceneManager() : nullptr; }
WorldRenderer* SceneEditor::worldRenderer() { return gpuRender() ? gpuRender()->worldRenderer() : nullptr; }
SceneLightingPasses& SceneEditor::lightingPasses() { return gpuRender()->lightingPasses(); }
const SceneManager* SceneEditor::sceneManager() const { return gpuRender() ? gpuRender()->sceneManager() : nullptr; }
WorldRenderer* SceneEditor::worldRenderer() const { return gpuRender() ? gpuRender()->worldRenderer() : nullptr; }
const SceneLightingPasses& SceneEditor::lightingPasses() const { return gpuRender()->lightingPasses(); }

std::shared_ptr<Material> SceneEditor::findMaterial(int materialID) const
{
    return m_app ? sceneSession::findMaterial(*m_app, materialID) : nullptr;
}

ecs::Entity SceneEditor::findEntityByInstanceIndex(int instanceIndex) const
{
    return m_app ? sceneSession::findEntityByInstanceIndex(*m_app, instanceIndex) : ecs::NullEntity;
}

const FirstPersonCamera& SceneEditor::currentCamera() const
{
    return m_app ? sceneSession::currentCamera(*m_app) : gpuRender()->camera().camera();
}

const std::shared_ptr<PlanarView>& SceneEditor::currentView() const
{
    return m_app ? sceneSession::currentView(*m_app) : gpuRender()->camera().view();
}

const DebugFeedbackStruct& SceneEditor::feedbackData() const
{
    return m_app ? sceneSession::feedbackData(*m_app) : worldRenderer()->getFeedbackData();
}

const DeltaTreeVizPathVertex* SceneEditor::debugDeltaPathTree() const
{
    return m_app ? sceneSession::debugDeltaPathTree(*m_app) : nullptr;
}

int SceneEditor::accumulationSampleIndex() const
{
    return m_app ? sceneSession::accumulationSampleIndex(*m_app) : 0;
}

math::uint2 SceneEditor::renderSize() const
{
    return m_app ? sceneSession::renderSize(*m_app) : uint2{ 0, 0 };
}

math::uint2 SceneEditor::displaySize() const
{
    return m_app ? sceneSession::displaySize(*m_app) : uint2{ 0, 0 };
}

float SceneEditor::avgTimePerFrame() const
{
    return m_app ? sceneSession::avgTimePerFrame(*m_app) : 0.f;
}

void SceneEditor::debugDrawLine(float3 start, float3 stop, float4 col1, float4 col2)
{
    if (m_app)
        sceneSession::debugDrawLine(*m_app, start, stop, col1, col2);
}

std::string SceneEditor::currentSceneName() const
{
    return m_app ? sceneSession::currentSceneName(*m_app) : std::string();
}

const std::vector<std::string>& SceneEditor::availableScenes() const
{
    static const std::vector<std::string> kEmpty;
    return m_app ? sceneSession::availableScenes(*m_app) : kEmpty;
}

void SceneEditor::setCurrentScene(const std::string& sceneName, bool forceReload)
{
    if (m_app)
        sceneSession::setCurrentScene(*m_app, sceneName, forceReload);
}

bool SceneEditor::loadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return m_app && sceneSession::loadGaussianSplatFile(*m_app, fileName, convertRdfToRub);
}

uint32_t SceneEditor::gaussianSplatCount() const
{
    return m_app ? sceneSession::gaussianSplatCount(*m_app) : 0;
}

uint32_t SceneEditor::gaussianSplatObjectCount() const
{
    return m_app ? sceneSession::gaussianSplatObjectCount(*m_app) : 0;
}

uint SceneEditor::sceneCameraCount() const
{
    return m_app ? sceneSession::sceneCameraCount(*m_app) : 1;
}

uint& SceneEditor::selectedCameraIndex()
{
    assert(m_app);
    return sceneSession::selectedCameraIndex(*m_app);
}

void SceneEditor::saveCurrentCamera() const
{
    if (m_app)
        sceneSession::saveCurrentCamera(*m_app);
}

void SceneEditor::loadCurrentCamera()
{
    if (m_app)
        sceneSession::loadCurrentCamera(*m_app);
}

std::string SceneEditor::currentCameraPosDirUp() const
{
    return m_app ? sceneSession::currentCameraPosDirUp(*m_app) : std::string();
}

bool SceneEditor::setCurrentCameraPosDirUp(const std::string& val)
{
    return m_app ? sceneSession::setCurrentCameraPosDirUp(*m_app, val) : false;
}

void SceneEditor::setCameraVerticalFOV(float cameraFOV)
{
    if (m_app)
        sceneSession::setCameraVerticalFOV(*m_app, cameraFOV);
}

float SceneEditor::cameraVerticalFOV() const
{
    return m_app ? sceneSession::cameraVerticalFOV(*m_app) : 0.f;
}

std::string SceneEditor::resolutionInfo() const
{
    return m_app ? sceneSession::resolutionInfo(*m_app) : "uninitialized";
}

std::string SceneEditor::fpsInfo() const
{
    return m_app ? sceneSession::fpsInfo(*m_app) : std::string();
}

const std::string& SceneEditor::envMapLocalPath() const
{
    static const std::string kEmpty;
    return m_app ? sceneSession::envMapLocalPath(*m_app) : kEmpty;
}

const std::string& SceneEditor::envMapOverrideSource() const
{
    static const std::string kEmpty;
    return m_app ? sceneSession::envMapOverrideSource(*m_app) : kEmpty;
}

const std::vector<std::filesystem::path>& SceneEditor::envMapMediaList()
{
    static const std::vector<std::filesystem::path> kEmpty;
    return m_app ? sceneSession::envMapMediaList(*m_app) : kEmpty;
}

void SceneEditor::setEnvMapOverrideSource(const std::string& envMapOverride)
{
    if (m_app)
        sceneSession::setEnvMapOverrideSource(*m_app, envMapOverride);
}

bool SceneEditor::hasAsyncLoadingInProgress() const
{
    return m_app && sceneSession::hasAsyncLoadingInProgress(*m_app);
}

bool SceneEditor::accumulationCompleted() const
{
    return m_app && sceneSession::accumulationCompleted(*m_app);
}

GLFWwindow* SceneEditor::glfwWindow() const
{
    return gpuDevice().GetWindow();
}

void SceneEditor::initStreamlineAndWindow()
{
    if (m_app)
        sceneSession::initStreamlineAndWindow(*m_app);
#if CAUSTICA_WITH_PYTHON
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

void SceneEditor::attachGpuRenderSubsystem(GpuRenderSubsystem& gpuRenderSubsystem)
{
    m_gpuRenderSubsystem = &gpuRenderSubsystem;
    if (m_app)
        sceneSession::attachGpuRenderSubsystem(*m_app, gpuRenderSubsystem);
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_viewState.progressLoading.Set(95);
}

void SceneEditor::initializeSession(const std::string& preferredScene)
{
    if (m_app)
        sceneSession::initializeSession(*m_app, preferredScene);
}

void SceneEditor::PrepareEditorFrame()
{
    HandleDroppedFiles();
    m_settings.DebugExploreDeltaTree = m_editor.ShowDeltaTree;
}

void SceneEditor::onSceneUnloading()
{
    m_editor.TogglableNodes = nullptr;
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

    if (m_sampleGame != nullptr)
        m_sampleGame->SceneUnloading();
}

void SceneEditor::onSceneLoadedEarly()
{
    if (m_sampleGame != nullptr)
    {
        const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
        m_sampleGame->SceneLoaded(
            sceneManager()->getScene(),
            sceneManager()->getCurrentScenePath(),
            assetsRoot);
    }
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (m_editor.TogglableNodes == nullptr)
    {
        auto activeScene = sceneManager()->getScene();
        auto* ew = activeScene ? activeScene->GetEntityWorld() : nullptr;
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

void SceneEditor::onSceneLoaded()
{
    if (m_app)
        sceneSession::onSceneLoaded(*m_app);
}

void SceneEditor::syncLoadedSceneSystems()
{
    if (!m_app || !sceneSession::isSceneLoaded(*m_app))
        return;

    const std::string loadedSceneName = currentSceneName();
    if (loadedSceneName.empty() || loadedSceneName == m_editorState.loadedSceneName)
        return;

    if (!m_editorState.loadedSceneName.empty())
        onSceneUnloading();

    m_editorState.loadedSceneName = loadedSceneName;
    onSceneLoadedEarly();
    onSceneLoadedBeforeGpuPrep();
    onSceneLoadedAfterCollectTextures();
    onSceneLoadedComplete();
}

void SceneEditor::onBeginFrameScheduled()
{
    CaptureScriptPreRender();
}

void SceneEditor::onAnimateBegin(float& fElapsedTimeSeconds)
{
    m_captureScriptManager->PreAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting && m_app && sceneSession::isSceneLoaded(*m_app))
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
        m_viewState.sceneTime = m_sampleGame->GetGameTime();
}

void SceneEditor::onAnimateGameCamera(float fElapsedTimeSeconds)
{
    if (m_sampleGame)
        m_sampleGame->TickCamera(fElapsedTimeSeconds, gpuRender()->camera().camera());
}

void SceneEditor::onAnimateEnd(float /*fElapsedTimeSeconds*/)
{
    m_captureScriptManager->PostAnim();
}

void SceneEditor::updateWindowTitle()
{
    if (!m_app)
        return;

    if (auto activeScene = scene())
    {
        SceneManager* manager = sceneManager();
        std::string extraInfo = ", " + sceneSession::fpsInfo(*m_app) + ", " + manager->getCurrentSceneName()
            + ", " + sceneSession::resolutionInfo(*m_app) + ", (L: " + std::to_string(activeScene->GetLightEntities().size())
            + ", MAT: " + std::to_string(activeScene->GetMaterials().size())
            + ", MESH: " + std::to_string(activeScene->GetMeshes().size())
            + ", I: " + std::to_string(activeScene->GetMeshInstances().size())
            + ", SI: " + std::to_string(activeScene->GetSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
            + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
            + ")";

        gpuDevice().SetInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
    }
}

bool SceneEditor::shouldRenderWhenUnfocused() const
{
    auto* wr = worldRenderer();
    if (!wr)
        return false;

    if (wr->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork())
        return true;

    if (m_editor.RenderWhenOutOfFocus)
        return true;

    return !m_settings.RealtimeMode && (wr->getAccumulationSampleIndex() < m_settings.AccumulationTarget);
}

void SceneEditor::setSceneTime(double sceneTime)
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_viewState.sceneTime = sceneTime;
}

double SceneEditor::sceneTime() const
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        return m_sampleGame->GetGameTime();
    return m_viewState.sceneTime;
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

std::vector<float3> SceneEditor::getMeshVertices(const std::shared_ptr<MeshInfo>& mesh) const
{
    return m_contentEditor.getMeshVertices(mesh);
}

std::vector<float3> SceneEditor::getMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh)
{
    return m_contentEditor.getMeshVerticesWorld(mesh);
}

std::vector<float3> SceneEditor::getMeshVerticesWorld(caustica::ecs::Entity entity)
{
    return m_contentEditor.getMeshVerticesWorld(entity);
}

void SceneEditor::setMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVerticesWorld(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void SceneEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVerticesWorld(entity, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void SceneEditor::setMeshVertices(const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVertices(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
}

ZoomTool* SceneEditor::GetOrCreateZoomTool()
{
    if (m_zoomTool == nullptr)
        m_zoomTool = std::make_unique<ZoomTool>(device(), gpuRender()->shaderFactory());
    return m_zoomTool.get();
}

bool SceneEditor::ShowDeltaTree() const
{
    return m_editor.ShowDeltaTree;
}

void SceneEditor::ResolvePickFeedback(const DebugFeedbackStruct& feedback)
{
    if (m_renderState.Picking.MaterialRequested)
        m_editor.SelectedMaterial = findMaterial(int(feedback.pickedMaterialID));
    if (m_renderState.Picking.InstanceRequested)
    {
        m_editor.SelectedEntity = findEntityByInstanceIndex(int(feedback.pickedInstanceIndex));
        if (m_editor.SelectedEntity != caustica::ecs::NullEntity)
            m_editor.SelectedGaussianSplat = false;
    }
}

void SceneEditor::afterWorldRender(GpuDevice& gpuDevice)
{
    auto* wr = worldRenderer();
    if (!wr)
        return;

    if (m_settings.ContinuousDebugFeedback || m_renderState.Picking.hasActivePickRequest())
        ResolvePickFeedback(wr->getFeedbackData());

    if (m_settings.ContinuousDebugFeedback || m_renderState.Picking.hasActivePickRequest())
        m_renderState.Picking.clearPickRequests();

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (!framebuffer)
            return false;
        nvrhi::ITexture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        auto* renderDevice = &gpuRender()->renderDevice();
        return SaveTextureToFile(
            gpuDevice.GetDevice(), *renderDevice, texture, nvrhi::ResourceStates::Common, fileName);
    };
    CaptureScriptPostRender(saveFramebuffer);

    if (ConsumeExperimentalPhotoScreenshot())
    {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (framebuffer)
            wr->denoisedScreenshot(framebuffer->getDesc().colorAttachments[0].texture);
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
