#include <engine/SceneRuntime.h>

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/WorldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include <render/Core/RenderTargets.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>

#include <core/path_utils.h>
#include <scene/scene_utils.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/BindingCache.h>
#include <render/Core/View.h>
#include <backend/GpuDevice.h>
#include <cassert>
#include <core/log.h>
#include <core/Timer.h>
#include <engine/Application.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/RenderThread.h>
#include <core/vfs/VFS.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <shaders/light_cb.h>

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

#ifdef _WIN32
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

const char* g_windowTitle = "caustica";
FPSLimiter g_FPSLimiter;

namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }
}

namespace caustica
{

SceneRuntime::SceneRuntime(const CommandLineOptions& cmdLine,
    render::RenderSessionState& sessionState,
    render::SessionDiagnostics& diagnostics)
    : m_cmdLine(cmdLine)
    , m_sessionState(sessionState)
    , m_settings(sessionState.settings)
    , m_renderState(sessionState.runtime)
    , m_sessionDiagnostics(diagnostics)
{
    m_progressLoading.Start("Initializing...");
    m_progressLoading.Set(50);
}

SceneRuntime::~SceneRuntime() = default;

void SceneRuntime::DebugDrawLine(float3 start, float3 stop, float4 col1, float4 col2)
{
    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
        return;
    auto& lines = worldRenderer->getCpuSideDebugLines();
    if (int(lines.size()) + 2 >= MAX_DEBUG_LINES)
        return;
    DebugLineStruct dls = { float4(start, 1), col1 };
    DebugLineStruct dle = { float4(stop, 1), col2 };
    lines.push_back(dls);
    lines.push_back(dle);
}

void SceneRuntime::bindGpuRenderSubsystem(GpuRenderSubsystem& gpuRenderSubsystem)
{
    m_gpuRenderSubsystem = &gpuRenderSubsystem;
    m_shaderFactory = gpuRenderSubsystem.shaderFactory();
    m_CommonPasses = gpuRenderSubsystem.commonPasses();
    m_bindingCache = gpuRenderSubsystem.bindingCache();
    m_DescriptorTable = gpuRenderSubsystem.descriptorTable();
    m_TextureLoader = gpuRenderSubsystem.textureLoader();
    m_sceneManager = gpuRenderSubsystem.sceneManager();
    m_lightingPasses = &gpuRenderSubsystem.lightingPasses();
    m_rayTracingResources = &gpuRenderSubsystem.rayTracingResources();
    m_gaussianSplatPasses = &gpuRenderSubsystem.gaussianSplatPasses();

    m_camera = &gpuRenderSubsystem.camera();
    m_cameraController.bind(*m_camera, m_settings, gpuRenderSubsystem.worldRenderer());
}

WorldRenderer* SceneRuntime::GetWorldRenderer() const
{
    return m_gpuRenderSubsystem ? m_gpuRenderSubsystem->worldRenderer() : nullptr;
}

SceneLightingPasses& SceneRuntime::GetLightingPasses()
{
    assert(m_lightingPasses != nullptr);
    return *m_lightingPasses;
}

const SceneLightingPasses& SceneRuntime::GetLightingPasses() const
{
    assert(m_lightingPasses != nullptr);
    return *m_lightingPasses;
}

const std::string& SceneRuntime::GetEnvMapLocalPath() const { return GetLightingPasses().envMapLocalPath(); }
const std::string& SceneRuntime::GetEnvMapOverrideSource() const { return GetLightingPasses().envMapOverride(); }
const std::vector<std::filesystem::path>& SceneRuntime::GetEnvMapMediaList() { return GetLightingPasses().envMapMediaList(); }

std::shared_ptr<CommonRenderPasses> SceneRuntime::GetCommonPasses() const { return m_CommonPasses; }
std::shared_ptr<DescriptorTableManager> SceneRuntime::GetDescriptorTable() const { return m_DescriptorTable; }
BindingCache& SceneRuntime::GetBindingCache() { return *m_bindingCache; }

SceneRayTracingResources& SceneRuntime::GetRayTracingResources()
{
    assert(m_rayTracingResources != nullptr);
    return *m_rayTracingResources;
}

const SceneRayTracingResources& SceneRuntime::GetRayTracingResources() const
{
    assert(m_rayTracingResources != nullptr);
    return *m_rayTracingResources;
}

SceneGaussianSplatPasses& SceneRuntime::GetGaussianSplatPasses()
{
    assert(m_gaussianSplatPasses != nullptr);
    return *m_gaussianSplatPasses;
}

const SceneGaussianSplatPasses& SceneRuntime::GetGaussianSplatPasses() const
{
    assert(m_gaussianSplatPasses != nullptr);
    return *m_gaussianSplatPasses;
}

void SceneRuntime::prepareRenderFrame()
{
    m_progressLoading.Stop();
    m_sessionDiagnostics.asyncLoadingInProgress = false;
}

std::shared_ptr<Scene> SceneRuntime::GetScene() const
{
    return m_sceneManager ? m_sceneManager->getScene() : nullptr;
}

const std::vector<std::string>& SceneRuntime::GetAvailableScenes() const
{
    static const std::vector<std::string> kEmpty;
    return m_sceneManager ? m_sceneManager->getAvailableScenes() : kEmpty;
}

std::string SceneRuntime::GetCurrentSceneName() const
{
    return m_sceneManager ? m_sceneManager->getCurrentSceneName() : std::string();
}

uint SceneRuntime::GetSceneCameraCount() const
{
    auto scene = GetScene();
    if (!scene)
        return 1;
    return (uint)scene->GetCameraEntities().size() + 1;
}

uint& SceneRuntime::SelectedCameraIndex()
{
    return m_camera->selectedCameraIndex();
}

float SceneRuntime::GetCameraVerticalFOV() const
{
    return m_cameraController.getVerticalFOV();
}

const FirstPersonCamera& SceneRuntime::GetCurrentCamera() const
{
    return m_cameraController.camera();
}

const std::shared_ptr<PlanarView>& SceneRuntime::GetCurrentView() const
{
    return m_cameraController.view();
}

const PlanarView& SceneRuntime::GetView() const
{
    return *m_cameraController.view();
}

void SceneRuntime::initStreamlineAndWindow()
{
#if CAUSTICA_WITH_STREAMLINE
    if (!GetGpuDevice().IsHeadless())
    {
        m_settings.IsDLSSSuported = GetGpuDevice().GetStreamline().IsDLSSAvailable();
        m_settings.IsDLSSFGSupported = GetGpuDevice().GetStreamline().IsDLSSGAvailable();
        m_settings.IsReflexSupported = GetGpuDevice().GetStreamline().IsReflexAvailable();
        m_settings.IsDLSSRRSupported = GetGpuDevice().GetStreamline().IsDLSSRRAvailable();
    }
#endif
}

void SceneRuntime::Init(const std::string& preferredScene,
    const std::shared_ptr<ShaderFactory>& /*shaderFactory*/)
{
    if (!m_shaderFactory || !m_bindingCache)
    {
        caustica::fatal("SceneRuntime::Init requires bindGpuRenderSubsystem");
        return;
    }

    if (!m_gpuRenderSubsystem)
    {
        caustica::fatal("SceneRuntime::Init requires bindGpuRenderSubsystem");
        return;
    }

    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
    {
        caustica::fatal("SceneRuntime::Init requires a path tracer world renderer");
        return;
    }

    if (!m_DescriptorTable || !m_TextureLoader)
    {
        caustica::fatal("SceneRuntime::Init requires descriptor table and texture cache");
        return;
    }

    if (!worldRenderer->getRenderTargets())
        worldRenderer->createDeviceResources();

    if (!m_sceneManager || !m_camera)
    {
        caustica::fatal("SceneRuntime::Init requires bindGpuRenderSubsystem");
        return;
    }

    m_settings.EnableGaussianSplats = true;
    m_settings.GaussianSplatDepthTest = m_cmdLine.GaussianSplatDepthTest;
    m_settings.GaussianSplatScale = m_cmdLine.GaussianSplatScale;
    m_settings.GaussianSplatAlphaScale = m_cmdLine.GaussianSplatAlphaScale;
    m_settings.GaussianSplatBrightness = m_cmdLine.GaussianSplatBrightness;
    m_settings.GaussianSplatAsEmitter = m_cmdLine.GaussianSplatAsEmitter;
    m_settings.GaussianSplatEmissionIntensity = m_cmdLine.GaussianSplatEmissionIntensity;
    m_settings.GaussianSplatEmissionMaxProxyCount = m_cmdLine.GaussianSplatEmissionMaxProxyCount;
    m_settings.GaussianSplatAlphaCullThreshold = m_cmdLine.GaussianSplatAlphaCullThreshold;

    onBeforeInitialSceneLoad();

    if (GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        GetLightingPasses().createOpacityMapsIfSupported(GetDevice(), m_DescriptorTable, m_TextureLoader, m_shaderFactory);

    m_sceneManager->discoverAvailableScenes(GetLocalPath(c_AssetsFolder));

    std::string scene;
    if (LooksLikeInlineSceneJson(preferredScene))
    {
        scene = preferredScene;
    }
    else
    {
        std::filesystem::path preferredScenePath(preferredScene);
        scene = (!preferredScene.empty() && (preferredScenePath.is_absolute() || std::filesystem::exists(preferredScenePath)))
            ? preferredScene
            : FindPreferredScene(m_sceneManager->getAvailableScenes(), preferredScene);
    }

    SetCurrentScene(scene);
}

void SceneRuntime::SetCurrentScene(const std::string& sceneName, bool forceReload)
{
    if (caustica::IsRenderThread())
    {
        std::lock_guard lock(m_pendingSceneSwitchMutex);
        m_pendingSceneSwitch = PendingSceneSwitch{ sceneName, forceReload };
        return;
    }

    applySceneSwitch(sceneName, forceReload);
}

void SceneRuntime::applySceneSwitch(const std::string& sceneName, bool forceReload)
{
    if (!m_sceneManager)
        return;

    if (!m_sceneManager->beginSceneSwitch(sceneName, GetLocalPath(c_AssetsFolder), forceReload))
        return;

    m_settings.ResetAccumulation = true;
    m_settings.ResetRealtimeCaches = true;
    m_sceneManager->setAsyncLoadingEnabled(false);

    m_progressLoading.Stop();
    m_progressLoading.Start("Loading scene...");
    m_sceneManager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        m_sceneManager->getCurrentScenePath());
    if (m_sceneManager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        m_sceneManager->clearScene();
        m_progressLoading.Stop();
    }
}

bool SceneRuntime::shouldSkipRender() const
{
    return !m_sceneManager || m_sceneManager->getScene() == nullptr;
}

void SceneRuntime::beginFrame()
{
    processPendingSceneSwitch();
    tickSceneSwitchTest();
}

bool SceneRuntime::processPendingSceneSwitch()
{
    std::optional<PendingSceneSwitch> pending;
    {
        std::lock_guard lock(m_pendingSceneSwitchMutex);
        pending.swap(m_pendingSceneSwitch);
    }

    if (!pending)
        return false;

    applySceneSwitch(pending->sceneName, pending->forceReload);
    return true;
}

void SceneRuntime::tickSceneSwitchTest()
{
    if (m_cmdLine.sceneSwitchTestInterval <= 0 || !m_sceneManager)
        return;

    if (--m_sceneSwitchTestFramesUntilSwitch > 0)
        return;

    m_sceneSwitchTestFramesUntilSwitch = m_cmdLine.sceneSwitchTestInterval;

    const std::vector<std::string>& scenes = GetAvailableScenes();
    if (scenes.size() < 2)
        return;

    if (m_sceneSwitchTestSceneIndex >= scenes.size())
        m_sceneSwitchTestSceneIndex = 0;

    const std::string& nextScene = scenes[m_sceneSwitchTestSceneIndex++];
    caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
    SetCurrentScene(nextScene);

    ++m_sceneSwitchTestSwitchesDone;
    if (m_cmdLine.sceneSwitchTestCount > 0
        && m_sceneSwitchTestSwitchesDone >= m_cmdLine.sceneSwitchTestCount
        && m_application)
    {
        m_application->requestExit();
    }
}

bool SceneRuntime::LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return GetGaussianSplatPasses().loadFromFile(fileName, convertRdfToRub);
}

uint32_t SceneRuntime::GetGaussianSplatCount() const
{
    return GetGaussianSplatPasses().splatCount();
}

uint32_t SceneRuntime::GetGaussianSplatObjectCount() const
{
    return GetGaussianSplatPasses().objectCount();
}

const std::string& SceneRuntime::GetGaussianSplatFileName() const
{
    return GetGaussianSplatPasses().fileNameSummary();
}

void SceneRuntime::SceneUnloading()
{
    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_uncompressedTextures.clear();

    runGpuWorkOnRenderThread([this]() {
        if (m_gpuRenderSubsystem)
            m_gpuRenderSubsystem->onSceneUnloading();
    });
}

void SceneRuntime::runGpuWorkOnRenderThread(const std::function<void()>& work)
{
    if (!work)
        return;
    if (m_application)
        m_application->runGpuWorkOnRenderThread(work);
    else
        work();
}

void SceneRuntime::syncCameraFromScene()
{
    auto scene = GetScene();
    if (!scene || !m_camera)
        return;

    const auto& cameraEntities = scene->GetCameraEntities();
    const auto* ew = scene->GetEntityWorld();
    bool syncedCamera = false;
    if (!cameraEntities.empty() && ew)
    {
        const uint32_t selectedIndex = m_camera->selectedCameraIndex();
        const uint32_t camIdx = (selectedIndex > 0) ? (selectedIndex - 1)
            : static_cast<uint32_t>(cameraEntities.size() - 1);
        if (camIdx < cameraEntities.size())
        {
            ecs::Entity camEntity = cameraEntities[camIdx];
            const auto* camComp = scene::TryGetCamera(ew->world(), camEntity);
            const auto* persData = camComp ? scene::TryGetPerspectiveCameraData(*camComp) : nullptr;
            const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
            if (persData && globalComp)
            {
                m_cameraController.syncFromSceneCamera(*persData, globalComp->transform);
                syncedCamera = true;
            }
        }
    }
    if (!syncedCamera)
        m_camera->setupDefaultCamera();
}

void SceneRuntime::SceneLoaded()
{
    if (!m_sceneManager || !m_gpuRenderSubsystem)
        return;

    const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
    m_gpuRenderSubsystem->refreshEnvironmentMapMediaList(assetsRoot, m_sceneManager->getCurrentScenePath());

    m_progressLoading.Set(50);

    onSceneLoadedEarly();

    m_progressLoading.Set(55);

    m_gpuRenderSubsystem->onSceneLoadedBegin();

    onSceneLoadedBeforeGpuPrep();

    runGpuWorkOnRenderThread([this]() {
        m_gpuRenderSubsystem->onSceneLoadedGpuPrep();
    });

    syncCameraFromScene();

    m_progressLoading.Set(60);

    m_gpuRenderSubsystem->onSceneLoadedGpuFinish();

    CollectUncompressedTextures();

    m_progressLoading.Set(70);

    onSceneLoadedAfterCollectTextures();

    m_progressLoading.Set(90);

    m_gpuRenderSubsystem->applyCmdLinePostLoadOverrides();
    if (!m_cmdLine.cameraPosDirUp.empty())
        SetCurrentCameraPosDirUp(m_cmdLine.cameraPosDirUp);

    m_camera->syncPreviousViewFromCurrent();

    m_progressLoading.Set(100);

    onSceneLoadedComplete();
}

bool SceneRuntime::IsSceneLoading() const
{
    return m_sceneManager && m_sceneManager->isSceneLoading();
}

bool SceneRuntime::IsSceneLoaded() const
{
    return m_sceneManager && m_sceneManager->isSceneLoaded();
}

void SceneRuntime::CollectUncompressedTextures()
{
    m_uncompressedTextures.clear();
    auto listUncompressedTextureIfNeeded = [&](std::shared_ptr<LoadedTexture> texture, bool normalMap)
    {
        if (texture == nullptr || texture->texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->texture->getDesc();
        if (nvrhi::getFormatInfo(desc.format).blockSize != 1)
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (nvrhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

        auto it = m_uncompressedTextures.insert(std::make_pair(texture, compressionType));
        if (!it.second)
        {
            assert(it.first->second == compressionType);
            return;
        }
    };
    GetLightingPasses().forEachUsedMaterialTexture([&](std::shared_ptr<LoadedTexture> texture, bool normalMap)
    {
        listUncompressedTextureIfNeeded(texture, normalMap);
    });
}

void SceneRuntime::Animate(float fElapsedTimeSeconds)
{
    if (m_settings.ActualFPSLimiter() > 0)
        fElapsedTimeSeconds = 1.0f / (float)m_settings.ActualFPSLimiter();

    onAnimateBegin(fElapsedTimeSeconds);

    m_lastDeltaTime = fElapsedTimeSeconds;

    if (m_sceneManager)
        m_sceneManager->updateLoading();

    m_camera->camera().SetMoveSpeed(m_settings.CameraMoveSpeed);

    if (m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest > 0)
    {
        m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest <= 0)
        {
            m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest = 0;
            m_renderState.Invalidation.ShaderReloadRequested = true;
            m_renderState.Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = m_settings.EnableAnimations && m_settings.RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || m_settings.ResetAccumulation;

    onAnimateGameTick(fElapsedTimeSeconds, enableAnimations);

    if (auto* worldRenderer = GetWorldRenderer())
    {
        if (auto* toneMappingPass = worldRenderer->getToneMappingPass())
            toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
    }

    if (IsSceneLoaded() && enableAnimationUpdate)
    {
        if (enableAnimations)
            m_sceneTime += fElapsedTimeSeconds;

        onAnimateUpdateSceneTime(fElapsedTimeSeconds, enableAnimations, enableAnimationUpdate);

        {
            auto* ew = m_sceneManager->getScene()->GetEntityWorld();
            if (ew)
            {
                auto& world = ew->world();
                for (ecs::Entity animEntity : m_sceneManager->getScene()->GetAnimationEntities())
                {
                    auto* animation = scene::TryGetAnimation(world, animEntity);
                    if (!animation || animation->channels.empty())
                        continue;

                    const float duration = scene::GetAnimationDuration(*animation);
                    if (duration <= 0.0f)
                        continue;

                    double cutLeft = 0.0;
                    double cutRight = 0.0;
                    const float animTime = (float)fmod(m_sceneTime + cutLeft, duration - cutLeft - cutRight);
                    (void)scene::ApplyAnimation(*animation, animTime, *ew);
                }

                if (enableAnimations)
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
            }
        }
    }
    else
    {
        m_sceneTime = 0.0f;
    }

    m_camera->selectedCameraIndex() = std::min(m_camera->selectedCameraIndex(), GetSceneCameraCount() - 1);
    if (m_camera->selectedCameraIndex() > 0)
    {
        auto scene = m_sceneManager->getScene();
        const auto& cameraEntities = scene->GetCameraEntities();
        const uint32_t camIdx = m_camera->selectedCameraIndex() - 1;
        const auto* ew = (camIdx < cameraEntities.size()) ? scene->GetEntityWorld() : nullptr;
        if (ew)
        {
            ecs::Entity camEntity = cameraEntities[camIdx];
            const auto* camComp = scene::TryGetCamera(ew->world(), camEntity);
            const auto* persData = camComp ? scene::TryGetPerspectiveCameraData(*camComp) : nullptr;
            const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
            if (persData && globalComp)
                m_cameraController.syncFromSceneCamera(*persData, globalComp->transform);
        }
    }

    m_camera->camera().Animate(fElapsedTimeSeconds);

    onAnimateGameCamera(fElapsedTimeSeconds);

    if (m_settings.CameraAntiRRSleepJitter > 0)
    {
        float off = 0.05f * ((GetWorldRenderer()->getFrameIndex() % 2) ? (-m_settings.CameraAntiRRSleepJitter) : (m_settings.CameraAntiRRSleepJitter));

        float3 dir = m_camera->camera().GetDir();
        float3 right = normalize(cross(dir, m_camera->camera().GetUp()));
        affine3 rot = rotation(right, off);
        dir = rot.transformVector(dir);

        m_camera->camera().LookTo(m_camera->camera().GetPosition(), dir, m_camera->camera().GetUp());
    }

    if (m_camera->cameraMovedSinceLastFrame())
    {
        m_camera->updateLastCameraState();
        if (!m_settings.RealtimeMode)
            m_settings.ResetAccumulation = true;
        GetWorldRenderer()->setGaussianSplatTemporalReset(true);
    }

    if (IsSceneLoaded() && m_sceneManager)
        m_sceneManager->tickSimulation(GetFrameIndex());

    onAnimateEnd(fElapsedTimeSeconds);

    double frameTime = GetGpuDevice().GetAverageFrameTimeSeconds();
    if (frameTime <= 0.0 && fElapsedTimeSeconds > 0.0f)
        frameTime = static_cast<double>(fElapsedTimeSeconds);
    UpdateFpsInfo(frameTime);

    updateWindowTitle();
}

void SceneRuntime::updateWindowTitle()
{
    if (auto scene = GetScene())
    {
        std::string extraInfo = ", " + m_fpsInfo + ", " + m_sceneManager->getCurrentSceneName() + ", " + GetResolutionInfo() + ", (L: " + std::to_string(scene->GetLightEntities().size()) + ", MAT: " + std::to_string(scene->GetMaterials().size())
            + ", MESH: " + std::to_string(scene->GetMeshes().size()) + ", I: " + std::to_string(scene->GetMeshInstances().size()) + ", SI: " + std::to_string(scene->GetSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
            + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
            + ")";

        GetGpuDevice().SetInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
    }
}

std::string SceneRuntime::GetResolutionInfo() const
{
    auto& r = *GetWorldRenderer();
    const auto* targets = r.getRenderTargets();
    if (targets == nullptr || targets->OutputColor == nullptr)
        return "uninitialized";
    const auto renderSize = r.getRenderSize();
    const auto displaySize = r.getDisplaySize();
    if (dm::all(renderSize == displaySize))
        return std::to_string(renderSize.x) + "x" + std::to_string(renderSize.y);
    return std::to_string(renderSize.x) + "x" + std::to_string(renderSize.y)
        + "->" + std::to_string(displaySize.x) + "x" + std::to_string(displaySize.y);
}

float SceneRuntime::GetAvgTimePerFrame() const
{
    if (m_sessionDiagnostics.benchFrames == 0)
        return 0.0f;
    std::chrono::duration<double> elapsed = (m_sessionDiagnostics.benchLast - m_sessionDiagnostics.benchStart);
    return float(elapsed.count() / m_sessionDiagnostics.benchFrames);
}

std::string SceneRuntime::GetCurrentCameraPosDirUp() const
{
    return m_cameraController.getPosDirUpString();
}

bool SceneRuntime::SetCurrentCameraPosDirUp(const std::string& val)
{
    return m_cameraController.setFromPosDirUpString(val);
}

void SceneRuntime::SetCameraVerticalFOV(float cameraFOV)
{
    m_cameraController.setVerticalFOV(cameraFOV);
}

void SceneRuntime::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    m_cameraController.setIntrinsics(fx, fy, cx, cy, width, height);
}

void SceneRuntime::ClearCameraIntrinsics()
{
    m_cameraController.clearIntrinsics();
}

void SceneRuntime::SaveCurrentCamera() const
{
    m_cameraController.saveToFile();
}

void SceneRuntime::LoadCurrentCamera()
{
    m_cameraController.loadFromFile();
}

void SceneRuntime::RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    GetRayTracingResources().requestMeshAccelRebuild(mesh);
}

void SceneRuntime::SetEnvMapOverrideSource(const std::string& envMapOverride)
{
    GetLightingPasses().setEnvMapOverrideSource(envMapOverride);
}

bool SceneRuntime::ShouldRenderUnfocused() const
{
    if (GetWorldRenderer()->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches)
        return true;

    return (!m_settings.RealtimeMode && (GetWorldRenderer()->getAccumulationSampleIndex() < m_settings.AccumulationTarget));
}

void SceneRuntime::SetSceneTime(double sceneTime)
{
    m_sceneTime = sceneTime;
}

double SceneRuntime::GetSceneTime()
{
    return m_sceneTime;
}

std::shared_ptr<Material> SceneRuntime::FindMaterial(int materialID) const
{
    return SceneManager::findMaterial(m_sceneManager->getScene(), materialID);
}

ecs::Entity SceneRuntime::FindEntityByInstanceIndex(int instanceIndex) const
{
    return SceneManager::findEntityByInstanceIndex(m_sceneManager->getScene(), instanceIndex);
}

nvrhi::ITexture* SceneRuntime::GetLdrColorTexture() const
{
    const auto* targets = GetWorldRenderer() ? GetWorldRenderer()->getRenderTargets() : nullptr;
    return targets ? targets->LdrColor.Get() : nullptr;
}

const DebugFeedbackStruct& SceneRuntime::GetFeedbackData() const
{
    static const DebugFeedbackStruct kEmpty{};
    return GetWorldRenderer() ? GetWorldRenderer()->getFeedbackData() : kEmpty;
}

const DeltaTreeVizPathVertex* SceneRuntime::GetDebugDeltaPathTree() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getDebugDeltaPathTree() : nullptr;
}

int SceneRuntime::GetAccumulationSampleIndex() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getAccumulationSampleIndex() : 0;
}

uint2 SceneRuntime::GetRenderSize() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getRenderSize() : uint2{ 0, 0 };
}

uint2 SceneRuntime::GetDisplaySize() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getDisplaySize() : uint2{ 0, 0 };
}

bool SceneRuntime::AccumulationCompleted() const
{
    return GetWorldRenderer() && GetWorldRenderer()->getAccumulationCompleted();
}

void SceneRuntime::recordFrameTiming(const GpuDevice& gpuDevice)
{
    double frameTime = gpuDevice.GetAverageFrameTimeSeconds();
    if (frameTime <= 0.0 && m_lastDeltaTime > 0.0f)
        frameTime = static_cast<double>(m_lastDeltaTime);
    UpdateFpsInfo(frameTime);
}

void SceneRuntime::UpdateFpsInfo(double frameTimeSeconds)
{
    if (frameTimeSeconds <= 0.0)
        return;

#if CAUSTICA_WITH_STREAMLINE
    if (m_settings.ActualDLSSFGMode() != SI::DLSSGMode::eOff)
    {
        uint32_t presentedFrames = m_settings.DLSSFGMultiplier;
        if (presentedFrames == 0)
            presentedFrames = 1u + m_settings.DLSSFGNumFramesToGenerate;

        m_fpsInfo = StringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G",
            frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
        return;
    }
#endif

    m_fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
}

void SceneRuntime::afterWorldRender(GpuDevice& /*gpuDevice*/)
{
    if (m_settings.ContinuousDebugFeedback || m_renderState.Picking.hasActivePickRequest())
        m_renderState.Picking.clearPickRequests();
}

void SceneRuntime::BackBufferResizing()
{
    if (auto* r = GetWorldRenderer())
        r->onBackBufferResizing();
}

} // namespace caustica
