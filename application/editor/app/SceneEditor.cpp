#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"

#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/TextureUtils.h>
#include <render/passes/debug/ZoomTool.h>
#include <render/SceneGaussianSplatPasses.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/App.h>
#include <engine/PathTracingRuntime.h>
#include <engine/RenderInfra.h>
#include <engine/SessionCamera.h>
#include "EditorAccess.h"
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <core/path_utils.h>
#include <scene/SceneEcs.h>
#include <scene/View.h>
#include <EditorUI.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

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


ecs::Entity SceneEditor::pickGaussianSplatAtPixel(math::uint2 renderPixel) const
{
    if (!m_app)
        return ecs::NullEntity;

    auto* pathTracing = caustica::pathTracingRuntime(*m_app);
    auto* entityWorld = caustica::entityWorld(*m_app);
    const auto& view = caustica::currentView(*m_app);
    if (!pathTracing || !entityWorld || !view)
        return ecs::NullEntity;

    const uint2 disp = caustica::displaySize(*m_app);
    const uint2 rend = caustica::renderSize(*m_app);
    if (disp.x == 0 || disp.y == 0 || rend.x == 0 || rend.y == 0)
        return ecs::NullEntity;

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

    for (const auto& object : pathTracing->gaussianSplatPasses().objects())
    {
        if (!ecs::isValid(object.entity) || !object.pass)
            continue;
        const auto* splatComp = entityWorld->world().tryGet<scene::GaussianSplatComponent>(object.entity);
        if (!splatComp || !splatComp->splat.enabled)
            continue;

        auto* boundsComp = entityWorld->world().tryGet<scene::BoundsComponent>(object.entity);
        box3 bbox = boundsComp ? boundsComp->globalBounds : box3::empty();
        if (bbox.isempty())
        {
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

void SceneEditor::bindSessionCameraSideEffects()
{
    if (m_app)
        caustica::bindSessionCameraSideEffects(*m_app);
#if CAUSTICA_WITH_PYTHON
    if (!m_pythonScripting)
        m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_viewState.progressLoading.Set(95);
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
    if (m_sampleGame == nullptr || !m_app)
        return;

    auto scene = caustica::activeScene(*m_app);
    if (!scene)
        return;

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    m_sampleGame->sceneLoaded(
        scene,
        caustica::currentScenePath(*m_app),
        assetsRoot);
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (m_editor.TogglableNodes != nullptr || !m_app)
        return;

    if (auto* ew = caustica::entityWorld(*m_app))
    {
        m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*m_editor.TogglableNodes, *ew, ew->root());
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

    const std::string loadedSceneName = caustica::currentSceneName(*m_app);
    if (loadedSceneName.empty() || loadedSceneName == m_editorState.loadedSceneName)
        return;

    m_editorState.loadedSceneName = loadedSceneName;
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
    auto* sessionCam = m_app ? caustica::sessionCameraResource(*m_app) : nullptr;
    if (m_sampleGame && sessionCam)
        m_sampleGame->TickCamera(fElapsedTimeSeconds, sessionCam->camera.camera());
}

void SceneEditor::onAnimateEnd(float /*fElapsedTimeSeconds*/)
{
    m_captureScriptManager->PostAnim();
}

void SceneEditor::updateWindowTitle()
{
    if (!m_app)
        return;

    auto* device = m_app->getGpuDevice();
    auto scene = caustica::activeScene(*m_app);
    if (!device || !scene)
        return;

    std::string extraInfo = ", " + caustica::fpsInfo(*m_app) + ", " + caustica::currentSceneName(*m_app)
        + ", " + caustica::resolutionInfo(*m_app) + ", (L: " + std::to_string(scene->getLightEntities().size())
        + ", MAT: " + std::to_string(scene->getMaterials().size())
        + ", MESH: " + std::to_string(scene->getMeshes().size())
        + ", I: " + std::to_string(scene->getMeshInstances().size())
        + ", SI: " + std::to_string(scene->getSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";

    device->setInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
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
    if (m_editor.PendingDeleteEntity == caustica::ecs::NullEntity || !m_app)
        return;

    const caustica::ecs::Entity entity = m_editor.PendingDeleteEntity;
    m_editor.PendingDeleteEntity = caustica::ecs::NullEntity;

    auto* ew = caustica::entityWorld(*m_app);
    if (!ew || !ew->world().isAlive(entity))
        return;

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
    auto* infra = m_app ? caustica::renderInfra(*m_app) : nullptr;
    auto* device = m_app ? m_app->getGpuDevice() : nullptr;
    if (m_zoomTool == nullptr && infra && infra->shaderFactory && device)
        m_zoomTool = std::make_unique<ZoomTool>(device->getDevice(), infra->shaderFactory);
    return m_zoomTool.get();
}

bool SceneEditor::showDeltaTree() const
{
    return m_editor.ShowDeltaTree;
}

void SceneEditor::resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick)
{
    if (!m_app)
        return;

    if (renderedPick.MaterialRequested)
        m_editor.SelectedMaterial = caustica::findMaterial(*m_app, int(feedback.pickedMaterialID));
    if (renderedPick.InstanceRequested)
    {
        ecs::Entity picked = caustica::findEntityByInstanceIndex(*m_app, int(feedback.pickedInstanceIndex));
        if (picked == ecs::NullEntity)
            picked = pickGaussianSplatAtPixel(renderedPick.Position);
        m_editor.SelectedEntity = picked;
        if (m_editor.SelectedEntity != caustica::ecs::NullEntity)
            m_editor.SelectedGaussianSplat = false;
    }
}

void SceneEditor::afterWorldRender(GpuDevice& gpuDevice)
{
    auto* wr = m_app ? caustica::worldRenderer(*m_app) : nullptr;
    if (!wr)
        return;

    const auto& renderedPick = wr->getLastRenderedPicking();
    if (m_settings.ContinuousDebugFeedback || renderedPick.hasActivePickRequest())
        resolvePickFeedback(wr->getFeedbackData(), renderedPick);

    if (renderedPick.MaterialRequested)
        m_renderState.Picking.MaterialRequested = false;
    if (renderedPick.InstanceRequested)
        m_renderState.Picking.InstanceRequested = false;

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        auto* infra = m_app ? caustica::renderInfra(*m_app) : nullptr;
        if (!framebuffer || !infra || !infra->renderDevice)
            return false;
        nvrhi::ITexture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        auto* renderDevice = infra->renderDevice.get();
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
