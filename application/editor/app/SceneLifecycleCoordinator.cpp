#include "SceneLifecycleCoordinator.h"

#include "EditorCameraController.h"

#include <EditorUI.h>
#include "common/LocalConfig.h"

#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <render/Core/SceneGpuUpdater.h>
#include <render/Core/PathTracerSettings.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/camera/Camera.h>
#include <scene/scene_utils.h>
#include <ecs/Entity.h>

#include <filesystem>

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

#include "game/GameScene.h"

namespace caustica::editor
{

SceneLifecycleCoordinator::SceneLifecycleCoordinator(Context context)
    : m_ctx(std::move(context))
{
}

void SceneLifecycleCoordinator::updateContext(Context context)
{
    m_ctx = std::move(context);
}

void SceneLifecycleCoordinator::refreshEnvironmentMapMediaList()
{
    if (!m_ctx.lightingPasses || !m_ctx.assetsRoot)
        return;

    const std::filesystem::path currentScenePath = m_ctx.sceneManager
        ? m_ctx.sceneManager->getCurrentScenePath()
        : std::filesystem::path();

    m_ctx.lightingPasses->refreshEnvironmentMapMediaList(
        m_ctx.assetsRoot(),
        currentScenePath);
}

bool SceneLifecycleCoordinator::setCurrentScene(const std::string& sceneName, bool forceReload)
{
    if (!m_ctx.sceneManager || !m_ctx.settings || !m_ctx.progressLoading || !m_ctx.assetsRoot)
        return false;

    if (!m_ctx.sceneManager->beginSceneSwitch(sceneName, m_ctx.assetsRoot(), forceReload))
        return false;

    m_ctx.settings->ResetAccumulation = true;
    m_ctx.sceneManager->setAsyncLoadingEnabled(false);

    m_ctx.progressLoading->Stop();
    m_ctx.progressLoading->Start("Loading scene...");
    m_ctx.sceneManager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        m_ctx.sceneManager->getCurrentScenePath());
    if (m_ctx.sceneManager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        m_ctx.sceneManager->clearScene();
        m_ctx.progressLoading->Stop();
        return false;
    }

    return true;
}

void SceneLifecycleCoordinator::onSceneUnloading()
{
    if (m_ctx.editor)
        m_ctx.editor->TogglableNodes = nullptr;

    if (m_ctx.worldRenderer)
        m_ctx.worldRenderer->onSceneUnloading();
    if (m_ctx.renderCore)
        m_ctx.renderCore->onSceneUnloading();
    if (m_ctx.bindingCache)
        m_ctx.bindingCache->Clear();

    if (m_ctx.lightingPasses)
        m_ctx.lightingPasses->sceneUnloading();

    if (m_ctx.editor)
    {
        m_ctx.editor->SelectedMaterial = nullptr;
        m_ctx.editor->SelectedEntity = caustica::ecs::NullEntity;
        m_ctx.editor->InspectorRotationEntity = caustica::ecs::NullEntity;
        m_ctx.editor->InspectorRotationEulerValid = false;
        m_ctx.editor->SelectedGaussianSplat = false;
    }

    if (m_ctx.gaussianSplatPasses && m_ctx.gaussianSplatPasses->isAttached())
        m_ctx.gaussianSplatPasses->sceneUnloading();

    if (m_ctx.settings)
        m_ctx.settings->EnvironmentMapParams = EnvironmentMapRuntimeParameters();

    if (m_ctx.uncompressedTextures)
        m_ctx.uncompressedTextures->clear();

    if (m_ctx.sampleGame != nullptr)
        m_ctx.sampleGame->SceneUnloading();
}

void SceneLifecycleCoordinator::onSceneLoaded()
{
    if (!m_ctx.sceneManager || !m_ctx.settings || !m_ctx.progressLoading)
        return;

    if (m_ctx.worldRenderer)
        m_ctx.worldRenderer->resetFrameIndex();

    refreshEnvironmentMapMediaList();

    m_ctx.progressLoading->Set(50);

    if (m_ctx.sampleGame != nullptr)
    {
        m_ctx.sampleGame->SceneLoaded(
            m_ctx.sceneManager->getScene(),
            m_ctx.sceneManager->getCurrentScenePath(),
            m_ctx.assetsRoot ? m_ctx.assetsRoot() : std::filesystem::path());
    }

    m_ctx.progressLoading->Set(55);

    if (m_ctx.textureLoader && m_ctx.commonPasses)
    {
        m_ctx.textureLoader->ProcessRenderingThreadCommands(*m_ctx.commonPasses, 0.f);
        m_ctx.textureLoader->LoadingFinished();
    }

    m_ctx.progressLoading->Set(60);

    if (m_ctx.sceneTime)
        *m_ctx.sceneTime = 0.f;

    if (m_ctx.frameIndex)
        caustica::render::SceneGpuUpdater::RefreshAfterLoad(*m_ctx.sceneManager->getScene(), m_ctx.frameIndex());

    if (m_ctx.gaussianSplatPasses && m_ctx.cmdLine)
        m_ctx.gaussianSplatPasses->onSceneLoaded(*m_ctx.cmdLine);

    m_ctx.progressLoading->Set(65);

    if (m_ctx.lightingPasses)
        m_ctx.lightingPasses->onSceneLoaded(*m_ctx.sceneManager->getScene(), *m_ctx.settings);

    m_ctx.settings->ToneMappingParams.exposureCompensation = 2.0f;
    m_ctx.settings->ToneMappingParams.exposureValue = 0.0f;

    if (m_ctx.editor && m_ctx.editor->TogglableNodes == nullptr)
    {
        auto scene = m_ctx.sceneManager->getScene();
        auto* ew = scene ? scene->GetEntityWorld() : nullptr;
        if (ew)
        {
            m_ctx.editor->TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
            UpdateTogglableNodes(*m_ctx.editor->TogglableNodes, *ew, ew->root());
        }
    }

    auto cameras = m_ctx.sceneManager->getScene()->GetCameras();
    auto camScene = cameras.empty() ? nullptr : std::dynamic_pointer_cast<caustica::PerspectiveCamera>(cameras.back());
    if (camScene && m_ctx.cameraController)
        m_ctx.cameraController->syncFromSceneCamera(camScene);
    else if (m_ctx.renderCore)
        m_ctx.renderCore->camera().setupDefaultCamera();

    if (m_ctx.renderCore && m_ctx.renderState)
    {
        m_ctx.renderCore->onSceneLoaded(
            *m_ctx.sceneManager->getScene(),
            m_ctx.renderState->Invalidation.AccelerationStructRebuildRequested);

        m_ctx.renderState->Invalidation.ShaderReloadRequested = true;
    }
    m_ctx.settings->EnableAnimations = false;
    m_ctx.settings->RealtimeMode = false;

    if (std::shared_ptr<SampleSettings> settings = m_ctx.sceneManager->getScene()->GetSampleSettingsNode())
    {
        m_ctx.settings->RealtimeMode = settings->realtimeMode.value_or(m_ctx.settings->RealtimeMode);
        m_ctx.settings->EnableAnimations = settings->enableAnimations.value_or(m_ctx.settings->EnableAnimations);
        if (settings->startingCamera.has_value() && m_ctx.renderCore)
            m_ctx.renderCore->camera().setSelectedCameraIndex(settings->startingCamera.value() + 1);
        if (settings->realtimeFireflyFilter.has_value())
        {
            m_ctx.settings->RealtimeFireflyFilterThreshold = settings->realtimeFireflyFilter.value();
            m_ctx.settings->RealtimeFireflyFilterEnabled = true;
        }
        m_ctx.settings->BounceCount = settings->maxBounces.value_or(m_ctx.settings->BounceCount);
        m_ctx.settings->DiffuseBounceCount = settings->maxDiffuseBounces.value_or(m_ctx.settings->DiffuseBounceCount);
        m_ctx.settings->TexLODBias = settings->textureMIPBias.value_or(m_ctx.settings->TexLODBias);
    }

    if (m_ctx.cmdLine && m_ctx.cmdLine->stopAnimations)
        m_ctx.settings->EnableAnimations = false;

    m_ctx.progressLoading->Set(70);

    if (m_ctx.postSceneLoad)
        m_ctx.postSceneLoad();

    m_ctx.progressLoading->Set(90);

    if (m_ctx.lightingPasses)
        m_ctx.lightingPasses->notifySceneReloaded(*m_ctx.sceneManager->getScene());

    m_ctx.progressLoading->Set(100);

    if (m_ctx.cmdLine)
    {
        if (m_ctx.cmdLine->OverrideToRealtimeMode)
            m_ctx.settings->RealtimeMode = true;
        if (m_ctx.cmdLine->OverrideToReferenceMode)
            m_ctx.settings->RealtimeMode = false;
        if (m_ctx.cmdLine->OverrideAutoexposureOff)
        {
            m_ctx.settings->ToneMappingParams.autoExposure = false;
            m_ctx.settings->ToneMappingParams.exposureValue = 0.0f;
        }
        if (m_ctx.cmdLine->OverrideExposureOffset != FLT_MAX)
            m_ctx.settings->ToneMappingParams.exposureCompensation = m_ctx.cmdLine->OverrideExposureOffset;
        if (m_ctx.cmdLine->DisableFireflyFilters)
        {
            m_ctx.settings->RealtimeFireflyFilterEnabled = false;
            m_ctx.settings->ReferenceFireflyFilterEnabled = false;
        }
        if (m_ctx.cmdLine->DisablePostProcessFilters)
            m_ctx.settings->EnableBloom = false;
        if (!m_ctx.cmdLine->cameraPosDirUp.empty() && m_ctx.setCameraPosDirUp)
            m_ctx.setCameraPosDirUp(m_ctx.cmdLine->cameraPosDirUp);
    }

    m_ctx.settings->MaterialVariantIndex = 0;

    if (m_ctx.diagnostics)
        m_ctx.diagnostics->asyncLoadingInProgress = true;

#if CAUSTICA_WITH_PYTHON
    if (m_ctx.pythonScripting && m_ctx.cmdLine
        && (!m_ctx.cmdLine->pythonScript.empty() || !m_ctx.cmdLine->pythonExpr.empty()))
    {
        if (m_ctx.pythonScripting->Initialize())
        {
            if (!m_ctx.cmdLine->pythonScript.empty())
                m_ctx.pythonScripting->QueueScriptFile(m_ctx.cmdLine->pythonScript);
            if (!m_ctx.cmdLine->pythonExpr.empty())
                m_ctx.pythonScripting->QueueScriptString(m_ctx.cmdLine->pythonExpr, "<--pythonExpr>");
        }
    }
#endif
}

} // namespace caustica::editor
