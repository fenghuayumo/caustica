#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"

#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/TextureUtils.h>
#include <render/passes/debug/ZoomTool.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/SceneGaussianSplatPasses.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <core/path_utils.h>
#include <scene/SceneEcs.h>
#include <scene/View.h>
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
    caustica::render::RenderAppState& renderAppState,
    EditorUIState& editorState,
    caustica::render::AppDiagnostics& diagnostics)
    : m_cmdLine(cmdLine)
    , m_renderAppState(renderAppState)
    , m_settings(renderAppState.settings)
    , m_renderState(renderAppState.runtime)
    , m_diagnostics(diagnostics)
    , m_editor(editorState)
    , m_selectionState(editorState)
    , m_editorCameraState(m_viewState)
    , m_inputRouter()
    , m_contentEditor(*this)
{
    m_viewState.progressLoading.start("Initializing...");
    m_viewState.progressLoading.Set(50);
    m_inputRouter.bind(*this);
    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_renderAppState, m_cmdLine);
    m_captureScriptState.manager = m_captureScriptManager.get();
}

SceneEditor::SceneEditor(const CommandLineOptions& cmdLine,
    EditorUIData& ui,
    caustica::render::AppDiagnostics& diagnostics)
    : SceneEditor(cmdLine, ui.render, ui.editor, diagnostics)
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
    return m_app ? m_app->tryResource<GpuRenderSubsystem>() : nullptr;
}

GpuDevice& SceneEditor::gpuDevice() const
{
    assert(m_app && m_app->getGpuDevice());
    return *m_app->getGpuDevice();
}

nvrhi::IDevice* SceneEditor::device() const
{
    return gpuDevice().getDevice();
}

uint32_t SceneEditor::frameIndex() const
{
    return gpuDevice().getFrameIndex();
}

std::shared_ptr<Scene> SceneEditor::scene() const
{
    return m_app ? caustica::activeScene(*m_app) : nullptr;
}

scene::SceneEntityWorld* SceneEditor::entityWorld() const
{
    return m_app ? caustica::entityWorld(*m_app) : nullptr;
}

bool SceneEditor::shouldSkipRender() const
{
    return m_app ? caustica::shouldSkipRender(*m_app) : true;
}

SceneManager* SceneEditor::sceneManager() { return gpuRender() ? gpuRender()->sceneManager() : nullptr; }
WorldRenderer* SceneEditor::worldRenderer() { return gpuRender() ? gpuRender()->worldRenderer() : nullptr; }
SceneLightingPasses& SceneEditor::lightingPasses() { return gpuRender()->lightingPasses(); }
const SceneManager* SceneEditor::sceneManager() const { return gpuRender() ? gpuRender()->sceneManager() : nullptr; }
WorldRenderer* SceneEditor::worldRenderer() const { return gpuRender() ? gpuRender()->worldRenderer() : nullptr; }
const SceneLightingPasses& SceneEditor::lightingPasses() const { return gpuRender()->lightingPasses(); }

std::shared_ptr<Material> SceneEditor::findMaterial(int materialID) const
{
    return m_app ? caustica::findMaterial(*m_app, materialID) : nullptr;
}

ecs::Entity SceneEditor::findEntityByInstanceIndex(int instanceIndex) const
{
    return m_app ? caustica::findEntityByInstanceIndex(*m_app, instanceIndex) : ecs::NullEntity;
}

ecs::Entity SceneEditor::pickGaussianSplatAtPixel(math::uint2 renderPixel) const
{
    auto* gpu = gpuRender();
    auto* entityWorld = this->entityWorld();
    const auto& view = currentView();
    if (!gpu || !entityWorld || !view)
        return ecs::NullEntity;

    const uint2 disp = displaySize();
    const uint2 rend = renderSize();
    if (disp.x == 0 || disp.y == 0 || rend.x == 0 || rend.y == 0)
        return ecs::NullEntity;

    // Picking.Position is in render pixels; project using display space for ImGui-style tests.
    const float2 mousePos = float2(
        float(renderPixel.x) * float(disp.x) / float(rend.x),
        float(renderPixel.y) * float(disp.y) / float(rend.y));
    const float2 displaySizeF = float2(float(disp.x), float(disp.y));
    const float4x4 viewProj = view->getViewProjectionMatrix();

    constexpr float2 kInvalidPos = float2(FLT_MAX, FLT_MAX);
    auto projectToScreen = [&](const float3& worldPos) -> float2
    {
        float4 projv = float4(worldPos, 1.f) * viewProj;
        if (std::fabs(projv.w) < 1e-8f)
            return kInvalidPos;
        projv /= projv.w;
        if (projv.z < 0.f)
            return kInvalidPos;
        projv.xy() = projv.xy() * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        projv.xy() *= displaySizeF;
        if (projv.x < 0.f || projv.x > displaySizeF.x || projv.y < 0.f || projv.y > displaySizeF.y)
            return kInvalidPos;
        return projv.xy();
    };

    ecs::Entity bestEntity = ecs::NullEntity;
    float bestDistance = FLT_MAX;

    for (const auto& object : gpu->gaussianSplatPasses().objects())
    {
        if (!object.splat || !object.splat->enabled || !ecs::isValid(object.entity) || !object.pass)
            continue;

        auto* boundsComp = entityWorld->world().tryGet<scene::BoundsComponent>(object.entity);
        box3 bbox = boundsComp ? boundsComp->globalBounds : box3::empty();
        if (bbox.isempty())
        {
            // Fallback: transform local splat AABB by the entity global transform.
            auto* global = entityWorld->world().tryGet<scene::GlobalTransformComponent>(object.entity);
            const box3 local = object.pass->getLocalBounds();
            if (global && !local.isempty())
                bbox = local * global->transformFloat;
        }
        if (bbox.isempty())
            continue;

        const float3 center = bbox.center();
        const float2 screenCenter = projectToScreen(center);
        if (screenCenter.x == FLT_MAX)
            continue;

        float screenRadius = 0.f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float2 screenCorner = projectToScreen(bbox.getCorner(corner));
            if (screenCorner.x == FLT_MAX)
                continue;
            screenRadius = std::max(screenRadius, length(screenCenter - screenCorner));
        }
        if (screenRadius <= 0.f)
            continue;

        screenRadius += 10.f;
        if (length(mousePos - screenCenter) > screenRadius)
            continue;

        const float range = length(center - view->getViewOrigin());
        if (range < bestDistance)
        {
            bestDistance = range;
            bestEntity = object.entity;
        }
    }

    return bestEntity;
}

const FirstPersonCamera& SceneEditor::currentCamera() const
{
    return m_app ? caustica::currentCamera(*m_app) : gpuRender()->camera().camera();
}

const std::shared_ptr<PlanarView>& SceneEditor::currentView() const
{
    return m_app ? caustica::currentView(*m_app) : gpuRender()->camera().view();
}

const DebugFeedbackStruct& SceneEditor::feedbackData() const
{
    return m_app ? caustica::feedbackData(*m_app) : worldRenderer()->getFeedbackData();
}

const DeltaTreeVizPathVertex* SceneEditor::debugDeltaPathTree() const
{
    return m_app ? caustica::debugDeltaPathTree(*m_app) : nullptr;
}

int SceneEditor::accumulationSampleIndex() const
{
    return m_app ? caustica::accumulationSampleIndex(*m_app) : 0;
}

math::uint2 SceneEditor::renderSize() const
{
    return m_app ? caustica::renderSize(*m_app) : uint2{ 0, 0 };
}

math::uint2 SceneEditor::displaySize() const
{
    return m_app ? caustica::displaySize(*m_app) : uint2{ 0, 0 };
}

float SceneEditor::avgTimePerFrame() const
{
    return m_app ? caustica::avgTimePerFrame(*m_app) : 0.f;
}

void SceneEditor::debugDrawLine(float3 start, float3 stop, float4 col1, float4 col2)
{
    if (m_app)
        caustica::debugDrawLine(*m_app, start, stop, col1, col2);
}

std::string SceneEditor::currentSceneName() const
{
    return m_app ? caustica::currentSceneName(*m_app) : std::string();
}

const std::vector<std::string>& SceneEditor::availableScenes() const
{
    static const std::vector<std::string> kEmpty;
    return m_app ? caustica::availableScenes(*m_app) : kEmpty;
}

void SceneEditor::setCurrentScene(const std::string& sceneName, bool forceReload)
{
    if (m_app)
        caustica::setCurrentScene(*m_app, sceneName, forceReload);
}

bool SceneEditor::loadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return m_app && caustica::loadGaussianSplatFile(*m_app, fileName, convertRdfToRub);
}

uint32_t SceneEditor::gaussianSplatCount() const
{
    return m_app ? caustica::gaussianSplatCount(*m_app) : 0;
}

uint32_t SceneEditor::gaussianSplatObjectCount() const
{
    return m_app ? caustica::gaussianSplatObjectCount(*m_app) : 0;
}

uint SceneEditor::sceneCameraCount() const
{
    return m_app ? caustica::sceneCameraCount(*m_app) : 1;
}

uint& SceneEditor::selectedCameraIndex()
{
    assert(m_app);
    return caustica::selectedCameraIndex(*m_app);
}

void SceneEditor::saveCurrentCamera() const
{
    if (m_app)
        caustica::saveCurrentCamera(*m_app);
}

void SceneEditor::loadCurrentCamera()
{
    if (m_app)
        caustica::loadCurrentCamera(*m_app);
}

std::string SceneEditor::currentCameraPosDirUp() const
{
    return m_app ? caustica::currentCameraPosDirUp(*m_app) : std::string();
}

bool SceneEditor::setCurrentCameraPosDirUp(const std::string& val)
{
    return m_app ? caustica::setCurrentCameraPosDirUp(*m_app, val) : false;
}

void SceneEditor::setCameraVerticalFOV(float cameraFOV)
{
    if (m_app)
        caustica::setCameraVerticalFOV(*m_app, cameraFOV);
}

float SceneEditor::cameraVerticalFOV() const
{
    return m_app ? caustica::cameraVerticalFOV(*m_app) : 0.f;
}

std::string SceneEditor::resolutionInfo() const
{
    return m_app ? caustica::resolutionInfo(*m_app) : "uninitialized";
}

std::string SceneEditor::fpsInfo() const
{
    return m_app ? caustica::fpsInfo(*m_app) : std::string();
}

const std::string& SceneEditor::envMapLocalPath() const
{
    static const std::string kEmpty;
    return m_app ? caustica::envMapLocalPath(*m_app) : kEmpty;
}

const std::string& SceneEditor::envMapOverrideSource() const
{
    static const std::string kEmpty;
    return m_app ? caustica::envMapOverrideSource(*m_app) : kEmpty;
}

const std::vector<std::filesystem::path>& SceneEditor::envMapMediaList()
{
    static const std::vector<std::filesystem::path> kEmpty;
    return m_app ? caustica::envMapMediaList(*m_app) : kEmpty;
}

void SceneEditor::setEnvMapOverrideSource(const std::string& envMapOverride)
{
    if (m_app)
        caustica::setEnvMapOverrideSource(*m_app, envMapOverride);
}

bool SceneEditor::hasAsyncLoadingInProgress() const
{
    return m_app && caustica::hasAsyncLoadingInProgress(*m_app);
}

bool SceneEditor::accumulationCompleted() const
{
    return m_app && caustica::accumulationCompleted(*m_app);
}

GLFWwindow* SceneEditor::glfwWindow() const
{
    return gpuDevice().getWindow();
}

void SceneEditor::initStreamlineAndWindow()
{
    if (m_app)
        caustica::initStreamlineAndWindow(*m_app);
#if CAUSTICA_WITH_PYTHON
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

void SceneEditor::attachGpuRenderSubsystem(GpuRenderSubsystem& gpuRenderSubsystem)
{
    m_gpuRenderSubsystem = &gpuRenderSubsystem;
    if (m_app)
        caustica::attachGpuRenderSubsystem(*m_app, gpuRenderSubsystem);
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_viewState.progressLoading.Set(95);
}

void SceneEditor::initializeScene(const std::string& preferredScene)
{
    if (m_app)
        caustica::initializeScene(*m_app, preferredScene);
}

void SceneEditor::prepareEditorFrame()
{
    m_settings.DebugExploreDeltaTree = m_editor.ShowDeltaTree;
}

void SceneEditor::onSceneUnloading()
{
    m_editor.TogglableNodes = nullptr;
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedEntity = caustica::ecs::NullEntity;
    m_editor.PendingDeleteEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

    if (m_sampleGame != nullptr)
        m_sampleGame->sceneUnloading();
}

void SceneEditor::onSceneLoadedEarly()
{
    if (m_sampleGame != nullptr)
    {
        const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
        m_sampleGame->sceneLoaded(
            sceneManager()->getScene(),
            sceneManager()->getCurrentScenePath(),
            assetsRoot);
    }
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (m_editor.TogglableNodes == nullptr)
    {
        if (auto* ew = entityWorld())
        {
            m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
            UpdateTogglableNodes(*m_editor.TogglableNodes, *ew, ew->root());
        }
    }
}

void SceneEditor::onSceneLoadedAfterCollectTextures()
{
    LocalConfig::PostSceneLoad(*this, m_renderAppState, m_editor);
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

void SceneEditor::onSceneLoadedFromLoader()
{
    onSceneLoadedEarly();
    onSceneLoadedBeforeGpuPrep();
    if (m_app)
        caustica::onSceneLoaded(*m_app);
    onSceneLoadedAfterCollectTextures();
    onSceneLoadedComplete();
}

void SceneEditor::onSceneLoaded()
{
    if (m_app)
        caustica::onSceneLoaded(*m_app);
}

void SceneEditor::syncLoadedSceneSystems()
{
    if (!m_app || !caustica::isSceneLoaded(*m_app))
        return;

    const std::string loadedSceneName = currentSceneName();
    if (loadedSceneName.empty() || loadedSceneName == m_editorState.loadedSceneName)
        return;

    m_editorState.loadedSceneName = loadedSceneName;
}

void SceneEditor::onBeginFrameScheduled()
{
    captureScriptPreRender();
}

void SceneEditor::onAnimateBegin(float& fElapsedTimeSeconds)
{
    m_captureScriptManager->preAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting && m_app && caustica::isSceneLoaded(*m_app))
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
        m_viewState.sceneTime = m_sampleGame->gameTime();
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
        std::string extraInfo = ", " + caustica::fpsInfo(*m_app) + ", " + manager->getCurrentSceneName()
            + ", " + caustica::resolutionInfo(*m_app) + ", (L: " + std::to_string(activeScene->getLightEntities().size())
            + ", MAT: " + std::to_string(activeScene->getMaterials().size())
            + ", MESH: " + std::to_string(activeScene->getMeshes().size())
            + ", I: " + std::to_string(activeScene->getMeshInstances().size())
            + ", SI: " + std::to_string(activeScene->getSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
            + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
            + ")";

        gpuDevice().setInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
    }
}

bool SceneEditor::shouldRenderWhenUnfocused() const
{
    auto* wr = worldRenderer();
    if (!wr)
        return false;

    if (wr->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->isDoingWork())
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
        return m_sampleGame->gameTime();
    return m_viewState.sceneTime;
}

void SceneEditor::handleDroppedFiles()
{
    m_contentEditor.handleDroppedFiles(m_editor.PendingDroppedFiles);
}

bool SceneEditor::loadMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadMeshFile(filePath);
}

bool SceneEditor::loadGltfMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadGltfMeshFile(filePath);
}

bool SceneEditor::loadObjMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadObjMeshFile(filePath);
}

bool SceneEditor::deleteSceneNode(caustica::ecs::Entity entity)
{
    return m_contentEditor.deleteSceneNode(entity);
}

void SceneEditor::processPendingSceneDeletes()
{
    if (m_editor.PendingDeleteEntity == caustica::ecs::NullEntity)
        return;

    const caustica::ecs::Entity entity = m_editor.PendingDeleteEntity;
    m_editor.PendingDeleteEntity = caustica::ecs::NullEntity;

    auto* ew = entityWorld();
    if (!ew || !ew->world().isAlive(entity))
        return;

    // despawn mutates ECS only; Extract flushes GPU/AS.
    deleteSceneNode(entity);
}

void SceneEditor::requestFullRebuild()
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

ZoomTool* SceneEditor::getOrCreateZoomTool()
{
    if (m_zoomTool == nullptr)
        m_zoomTool = std::make_unique<ZoomTool>(device(), gpuRender()->shaderFactory());
    return m_zoomTool.get();
}

bool SceneEditor::showDeltaTree() const
{
    return m_editor.ShowDeltaTree;
}

void SceneEditor::resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick)
{
    // pickedMaterialID is PTMaterial::gpuDataIndex for the hit geometry (sub-mesh),
    // not a per-instance material. Use the rendered-frame pick flags, not live UI state.
    if (renderedPick.MaterialRequested)
        m_editor.SelectedMaterial = findMaterial(int(feedback.pickedMaterialID));
    if (renderedPick.InstanceRequested)
    {
        ecs::Entity picked = findEntityByInstanceIndex(int(feedback.pickedInstanceIndex));
        if (picked == ecs::NullEntity)
            picked = pickGaussianSplatAtPixel(renderedPick.Position);
        m_editor.SelectedEntity = picked;
        if (m_editor.SelectedEntity != caustica::ecs::NullEntity)
            m_editor.SelectedGaussianSplat = false;
    }
}

void SceneEditor::afterWorldRender(GpuDevice& gpuDevice)
{
    auto* wr = worldRenderer();
    if (!wr)
        return;

    const auto& renderedPick = wr->getLastRenderedPicking();
    if (m_settings.ContinuousDebugFeedback || renderedPick.hasActivePickRequest())
        resolvePickFeedback(wr->getFeedbackData(), renderedPick);

    // Clear only the requests this finished frame actually owned. Clearing live
    // state from an older in-flight frame would drop a newer click.
    if (renderedPick.MaterialRequested)
        m_renderState.Picking.MaterialRequested = false;
    if (renderedPick.InstanceRequested)
        m_renderState.Picking.InstanceRequested = false;

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        if (!framebuffer)
            return false;
        nvrhi::ITexture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        auto* renderDevice = &gpuRender()->renderDevice();
        return saveTextureToFile(
            gpuDevice.getDevice(), *renderDevice, texture, nvrhi::ResourceStates::Common, fileName);
    };
    captureScriptPostRender(saveFramebuffer);

    if (consumeExperimentalPhotoScreenshot())
    {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        if (framebuffer)
            wr->denoisedScreenshot(framebuffer->getDesc().colorAttachments[0].texture);
    }
}

bool SceneEditor::consumeExperimentalPhotoScreenshot()
{
    if (!m_editor.ExperimentalPhotoModeScreenshot)
        return false;
    m_editor.ExperimentalPhotoModeScreenshot = false;
    return true;
}

void SceneEditor::captureScriptPreRender()
{
    if (m_captureScriptManager)
        m_captureScriptManager->preRender();
}

void SceneEditor::captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture)
{
    if (m_captureScriptManager)
        m_captureScriptManager->postRender(saveTexture);
}

void SceneEditor::onEvent(caustica::Event& event)
{
    m_inputRouter.onEvent(event);
}

} // namespace caustica::editor
