#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "EditorCameraController.h"

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>

#include <render/WorldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include "render/Core/RenderTargets.h"
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <events/key_event.h>
#include <events/mouse_event.h>

#include <assets/AssetSystem.h>
#include <scene/loader/RuntimeMeshLoader.h>
#include <render/Core/SceneMeshEditing.h>
#include <scene/SceneRuntimeMutation.h>
#include <render/Core/LightingUpdate.h>

#include <core/path_utils.h>
#include <scene/scene_utils.h>
#include <scene/SceneAnimationAccess.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/BindingCache.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/Core/View.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <engine/Application.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/RenderThread.h>
#include <core/json.h>
#include <core/vfs/VFS.h>
#include <EditorUI.h>
#include "common/LocalConfig.h"
#include <scene/SceneCameraAccess.h>
#include <ecs/Entity.h>
#include <math/float.h>
#include <math/math.h>
#include <shaders/light_cb.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>
#include <rhi/common/misc.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>


#include <render/Core/ComputePipelineRegistry.h>

#include "render/Core/AccelerationStructureUtil.h"

#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingCache.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>

#include <render/Passes/OMM/OpacityMicromapBuilder.h>

#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"
#include <render/Passes/Debug/Korgi.h>

#include <render/Passes/Debug/ZoomTool.h>

#include <render/Passes/PostProcess/DenoisingGuidesPass.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>

#include <scene/SceneEcs.h>
#include <scene/SceneCameraAccess.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include "game/GameScene.h"

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

#include <fstream>
#include <iostream>

#include <thread>

#ifdef _WIN32
// Use discrete GPU by default on laptops.
extern "C"
{
    // http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;

    // https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace caustica::editor
{

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

namespace
{
#if CAUSTICA_WITH_NATIVE_DLSS
    float GetNativeDLSSResolutionScale(SI::DLSSMode mode)
    {
        switch (mode)
        {
        case SI::DLSSMode::eUltraPerformance: return 1.0f / 3.0f;
        case SI::DLSSMode::eMaxPerformance:   return 0.5f;
        case SI::DLSSMode::eBalanced:         return 0.58f;
        case SI::DLSSMode::eMaxQuality:       return 2.0f / 3.0f;
        case SI::DLSSMode::eUltraQuality:     return 0.77f;
        case SI::DLSSMode::eDLAA:             return 1.0f;
        default:                              return 0.58f;
        }
    }

    uint2 GetNativeDLSSRenderSize(uint2 displaySize, SI::DLSSMode mode)
    {
        const float scale = GetNativeDLSSResolutionScale(mode);
        return uint2(
            std::max(1u, uint32_t(std::round(float(displaySize.x) * scale))),
            std::max(1u, uint32_t(std::round(float(displaySize.y) * scale))));
    }
#endif

    float4x4 MakePinholeIntrinsicsProjection(float fx, float fy, float cx, float cy, float width, float height, float zNear)
    {
        width = std::max(width, 1.0f);
        height = std::max(height, 1.0f);

        const float xScale = 2.0f * fx / width;
        const float yScale = 2.0f * fy / height;
        const float xOffset = 2.0f * cx / width - 1.0f;
        const float yOffset = 1.0f - 2.0f * cy / height;

        return float4x4(
            xScale, 0.0f, 0.0f, 0.0f,
            0.0f, yScale, 0.0f, 0.0f,
            xOffset, yOffset, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f);
    }
}

} // namespace caustica::editor

const char* g_windowTitle = "caustica";

const float c_envMapRadianceScale = 1.0f / 4.0f;

FPSLimiter g_FPSLimiter;

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
    , m_editor(editorState)
    , m_sessionDiagnostics(diagnostics)
    , m_inputRouter()
    , m_contentEditor(*this)
{
    m_progressLoading.Start("Initializing...");
    m_progressLoading.Set(50);

    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_sessionState, m_cmdLine);
}

SceneEditor::SceneEditor(const CommandLineOptions& cmdLine,
    EditorUIData& ui,
    caustica::render::SessionDiagnostics& diagnostics)
    : SceneEditor(cmdLine, ui.session, ui.editor, diagnostics)
{
    m_editorUi = &ui;
}

void SceneEditor::initStreamlineAndWindow()
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
#if CAUSTICA_WITH_PYTHON
    // Embedded Python scripting host - we always create the wrapper but the
    // interpreter itself is initialized on demand the first time a script
    // gets queued.  This keeps cold-start overhead at zero when scripting is
    // unused even if the executable was built with CAUSTICA_WITH_PYTHON=ON.
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

SceneEditor::~SceneEditor()
{
#if CAUSTICA_WITH_PYTHON
    // Tear down the Python interpreter first so that any nb::class_<>-bound
    // C++ objects (materials, lights, ...) are released while their owning
    // C++ data is still alive.
    m_pythonScripting.reset();
#endif
}

void SceneEditor::DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 )
{
    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
        return;
    auto& lines = worldRenderer->getCpuSideDebugLines();
    if (int(lines.size()) + 2 >= MAX_DEBUG_LINES) return;
    DebugLineStruct dls = { float4(start, 1), col1 };
    DebugLineStruct dle = { float4(stop, 1), col2 };
    lines.push_back(dls);
    lines.push_back(dle);
}

void SceneEditor::bindGpuRenderSubsystem(caustica::GpuRenderSubsystem& gpuRenderSubsystem)
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
    m_inputRouter.bind(*this);
}

caustica::render::WorldRenderer* SceneEditor::GetWorldRenderer() const
{
    return m_gpuRenderSubsystem ? m_gpuRenderSubsystem->worldRenderer() : nullptr;
}

caustica::render::SceneLightingPasses& SceneEditor::GetLightingPasses()
{
    assert(m_lightingPasses != nullptr);
    return *m_lightingPasses;
}

const caustica::render::SceneLightingPasses& SceneEditor::GetLightingPasses() const
{
    assert(m_lightingPasses != nullptr);
    return *m_lightingPasses;
}

const std::string& SceneEditor::GetEnvMapLocalPath() const { return GetLightingPasses().envMapLocalPath(); }
const std::string& SceneEditor::GetEnvMapOverrideSource() const { return GetLightingPasses().envMapOverride(); }
const std::vector<std::filesystem::path>& SceneEditor::GetEnvMapMediaList() { return GetLightingPasses().envMapMediaList(); }

std::shared_ptr<caustica::CommonRenderPasses> SceneEditor::GetCommonPasses() const { return m_CommonPasses; }
std::shared_ptr<caustica::DescriptorTableManager> SceneEditor::GetDescriptorTable() const { return m_DescriptorTable; }
caustica::BindingCache& SceneEditor::GetBindingCache() { return *m_bindingCache; }

caustica::render::SceneRayTracingResources& SceneEditor::GetRayTracingResources()
{
    assert(m_rayTracingResources != nullptr);
    return *m_rayTracingResources;
}

const caustica::render::SceneRayTracingResources& SceneEditor::GetRayTracingResources() const
{
    assert(m_rayTracingResources != nullptr);
    return *m_rayTracingResources;
}

caustica::render::SceneGaussianSplatPasses& SceneEditor::GetGaussianSplatPasses()
{
    assert(m_gaussianSplatPasses != nullptr);
    return *m_gaussianSplatPasses;
}

const caustica::render::SceneGaussianSplatPasses& SceneEditor::GetGaussianSplatPasses() const
{
    assert(m_gaussianSplatPasses != nullptr);
    return *m_gaussianSplatPasses;
}

void SceneEditor::PrepareEditorFrame()
{
    m_progressLoading.Stop();
    m_sessionDiagnostics.asyncLoadingInProgress = false;
    HandleDroppedFiles();
    m_settings.DebugExploreDeltaTree = m_editor.ShowDeltaTree;
}

std::shared_ptr<caustica::Scene> SceneEditor::GetScene() const
{
    return m_sceneManager ? m_sceneManager->getScene() : nullptr;
}

const std::vector<std::string>& SceneEditor::GetAvailableScenes() const
{
    static const std::vector<std::string> kEmpty;
    return m_sceneManager ? m_sceneManager->getAvailableScenes() : kEmpty;
}

std::string SceneEditor::GetCurrentSceneName() const
{
    return m_sceneManager ? m_sceneManager->getCurrentSceneName() : std::string();
}

uint SceneEditor::GetSceneCameraCount() const
{
    auto scene = GetScene();
    if (!scene)
        return 1;
    return (uint)scene->GetCameraEntities().size() + 1;
}

uint& SceneEditor::SelectedCameraIndex()
{
    return m_camera->selectedCameraIndex();
}

float SceneEditor::GetCameraVerticalFOV() const
{
    return m_cameraController.getVerticalFOV();
}

const caustica::FirstPersonCamera& SceneEditor::GetCurrentCamera() const
{
    return m_cameraController.camera();
}

const std::shared_ptr<caustica::PlanarView>& SceneEditor::GetCurrentView() const
{
    return m_cameraController.view();
}

const caustica::PlanarView& SceneEditor::GetView() const
{
    return *m_cameraController.view();
}

void SceneEditor::Init(const std::string& preferredScene,
    const std::shared_ptr<caustica::ShaderFactory>& /*shaderFactory*/)
{
    if (!m_shaderFactory || !m_bindingCache)
    {
        caustica::fatal("SceneEditor::Init requires bindGpuRenderSubsystem");
        return;
    }

    if (!m_gpuRenderSubsystem)
    {
        caustica::fatal("SceneEditor::Init requires bindGpuRenderSubsystem");
        return;
    }

    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
    {
        caustica::fatal("SceneEditor::Init requires a path tracer world renderer");
        return;
    }

    if (!m_DescriptorTable || !m_TextureLoader)
    {
        caustica::fatal("SceneEditor::Init requires descriptor table and texture cache");
        return;
    }

    if (!worldRenderer->getRenderTargets())
        worldRenderer->createDeviceResources();

    if (!m_sceneManager || !m_camera)
    {
        caustica::fatal("SceneEditor::Init requires bindGpuRenderSubsystem");
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
    
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_progressLoading.Set(95);

    if (GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        GetLightingPasses().createOpacityMapsIfSupported(GetDevice(), m_DescriptorTable, m_TextureLoader, m_shaderFactory);

    // Get all scenes in "assets" folder
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

    // Select initial scene
    SetCurrentScene(scene);
}

void SceneEditor::SetCurrentScene( const std::string & sceneName, bool forceReload )
{
    if (caustica::IsRenderThread())
    {
        std::lock_guard lock(m_pendingSceneSwitchMutex);
        m_pendingSceneSwitch = PendingSceneSwitch{sceneName, forceReload};
        return;
    }

    applySceneSwitch(sceneName, forceReload);
}

void SceneEditor::applySceneSwitch(const std::string& sceneName, bool forceReload)
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

bool SceneEditor::shouldSkipRender() const
{
    return !m_sceneManager || m_sceneManager->getScene() == nullptr;
}

void SceneEditor::beginFrame()
{
    processPendingSceneSwitch();
    tickSceneSwitchTest();
}

bool SceneEditor::processPendingSceneSwitch()
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

void SceneEditor::tickSceneSwitchTest()
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

bool SceneEditor::LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return GetGaussianSplatPasses().loadFromFile(fileName, convertRdfToRub);
}

uint32_t SceneEditor::GetGaussianSplatCount() const
{
    return GetGaussianSplatPasses().splatCount();
}

uint32_t SceneEditor::GetGaussianSplatObjectCount() const
{
    return GetGaussianSplatPasses().objectCount();
}

const std::string& SceneEditor::GetGaussianSplatFileName() const
{
    return GetGaussianSplatPasses().fileNameSummary();
}

void SceneEditor::SceneUnloading()
{
    m_editor.TogglableNodes = nullptr;
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_uncompressedTextures.clear();

    if (m_sampleGame != nullptr)
        m_sampleGame->SceneUnloading();

    runGpuWorkOnRenderThread([this]() {
        if (m_gpuRenderSubsystem)
            m_gpuRenderSubsystem->onSceneUnloading();
    });
}

void SceneEditor::runGpuWorkOnRenderThread(const std::function<void()>& work)
{
    if (!work)
        return;
    if (m_application)
        m_application->runGpuWorkOnRenderThread(work);
    else
        work();
}

void SceneEditor::syncCameraFromScene()
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

void SceneEditor::SceneLoaded()
{
    if (!m_sceneManager || !m_gpuRenderSubsystem)
        return;

    const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
    m_gpuRenderSubsystem->refreshEnvironmentMapMediaList(assetsRoot, m_sceneManager->getCurrentScenePath());

    m_progressLoading.Set(50);

    if (m_sampleGame != nullptr)
    {
        m_sampleGame->SceneLoaded(
            m_sceneManager->getScene(),
            m_sceneManager->getCurrentScenePath(),
            assetsRoot);
    }

    m_progressLoading.Set(55);

    m_gpuRenderSubsystem->onSceneLoadedBegin();

    if (m_editor.TogglableNodes == nullptr)
    {
        auto scene = m_sceneManager->getScene();
        auto* ew = scene ? scene->GetEntityWorld() : nullptr;
        if (ew)
        {
            m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
            UpdateTogglableNodes(*m_editor.TogglableNodes, *ew, ew->root());
        }
    }

    runGpuWorkOnRenderThread([this]() {
        m_gpuRenderSubsystem->onSceneLoadedGpuPrep();
    });

    syncCameraFromScene();

    m_progressLoading.Set(60);

    m_gpuRenderSubsystem->onSceneLoadedGpuFinish();

    CollectUncompressedTextures();

    m_progressLoading.Set(70);

    LocalConfig::PostSceneLoad(*this, m_sessionState, m_editor);

    m_progressLoading.Set(90);

    m_gpuRenderSubsystem->applyCmdLinePostLoadOverrides();
    if (!m_cmdLine.cameraPosDirUp.empty())
        SetCurrentCameraPosDirUp(m_cmdLine.cameraPosDirUp);

    m_camera->syncPreviousViewFromCurrent();

    m_progressLoading.Set(100);

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

bool SceneEditor::IsSceneLoading() const
{
    return m_sceneManager && m_sceneManager->isSceneLoading();
}

bool SceneEditor::IsSceneLoaded() const
{
    return m_sceneManager && m_sceneManager->isSceneLoaded();
}

void SceneEditor::CollectUncompressedTextures()
{
    // Make a list of uncompressed textures
    m_uncompressedTextures.clear();
    auto listUncompressedTextureIfNeeded = [ & ](std::shared_ptr<LoadedTexture> texture, bool normalMap)//, TextureCompressionType compressionType)
    {
        if (texture == nullptr || texture->texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->texture->getDesc();
        if (nvrhi::getFormatInfo(desc.format).blockSize != 1) // it's compressed, everything is fine!
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (nvrhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

        auto it = m_uncompressedTextures.insert(std::make_pair(texture, compressionType));
        if (!it.second)
        {
            assert(it.first->second == compressionType); // not the same compression type? that's bad!
            return;
        }
    };
    GetLightingPasses().forEachUsedMaterialTexture([&](std::shared_ptr<LoadedTexture> texture, bool normalMap)
    {
        listUncompressedTextureIfNeeded(texture, normalMap);
    });
}

void SceneEditor::Animate(float fElapsedTimeSeconds)
{
    if (m_settings.ActualFPSLimiter()>0)    // essential for stable video recording
        fElapsedTimeSeconds = 1.0f / (float)m_settings.ActualFPSLimiter();

    m_captureScriptManager->PreAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    // Drain any pending Python scripts. We do this on the renderer thread so
    // bindings observe a coherent scene state and so they can mutate UI
    // settings before rendering for the current frame happens.
    if (m_pythonScripting && IsSceneLoaded())
        m_pythonScripting->ProcessPendingScripts();
#endif

    m_lastDeltaTime = fElapsedTimeSeconds;

    if (m_sceneManager)
        m_sceneManager->updateLoading();

    m_camera->camera().SetMoveSpeed(m_settings.CameraMoveSpeed);

    if( m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest > 0 )
    {
        m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest <= 0 )
        {
            m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest = 0;
            m_renderState.Invalidation.ShaderReloadRequested = true;
            m_renderState.Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = m_settings.EnableAnimations && m_settings.RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || m_settings.ResetAccumulation;

    if (m_sampleGame) m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);

    if (auto* worldRenderer = GetWorldRenderer())
    {
        if (auto* toneMappingPass = worldRenderer->getToneMappingPass())
            toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
    }

    if (IsSceneLoaded() && enableAnimationUpdate)
    {
        if (enableAnimations)
            m_sceneTime += fElapsedTimeSeconds;
        if (m_sampleGame && m_sampleGame->IsInitialized())
            m_sceneTime = m_sampleGame->GetGameTime();

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

    m_camera->selectedCameraIndex() = std::min( m_camera->selectedCameraIndex(), GetSceneCameraCount()-1 );
    if (m_camera->selectedCameraIndex() > 0)
    {
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
    }

    m_camera->camera().Animate(fElapsedTimeSeconds);

    if (m_sampleGame) m_sampleGame->TickCamera(fElapsedTimeSeconds, m_camera->camera());

    if (m_settings.CameraAntiRRSleepJitter>0)
    {
        float off = 0.05f * ((GetWorldRenderer()->getFrameIndex()%2)?(-m_settings.CameraAntiRRSleepJitter):(m_settings.CameraAntiRRSleepJitter));

        float3 dir = m_camera->camera().GetDir();
        float3 right = normalize(cross(dir, m_camera->camera().GetUp()));
        affine3 rot = rotation(right, off);
        dir = rot.transformVector(dir);

        m_camera->camera().LookTo( m_camera->camera().GetPosition(), dir, m_camera->camera().GetUp() );
    }

    if (m_camera->cameraMovedSinceLastFrame())
    {
        m_camera->updateLastCameraState();
        if( !m_settings.RealtimeMode )
            m_settings.ResetAccumulation = true;
        GetWorldRenderer()->setGaussianSplatTemporalReset(true);
    }

    if (IsSceneLoaded() && m_sceneManager)
        m_sceneManager->tickSimulation(GetFrameIndex());

    m_captureScriptManager->PostAnim();

    double frameTime = GetGpuDevice().GetAverageFrameTimeSeconds();
    if (frameTime <= 0.0 && fElapsedTimeSeconds > 0.0f)
        frameTime = static_cast<double>(fElapsedTimeSeconds);
    UpdateFpsInfo(frameTime);

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

std::string SceneEditor::GetResolutionInfo() const
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

float SceneEditor::GetAvgTimePerFrame() const
{
    if (m_sessionDiagnostics.benchFrames == 0) return 0.0f;
    std::chrono::duration<double> elapsed = (m_sessionDiagnostics.benchLast - m_sessionDiagnostics.benchStart);
    return float(elapsed.count() / m_sessionDiagnostics.benchFrames);
}

std::string SceneEditor::GetCurrentCameraPosDirUp() const
{
    return m_cameraController.getPosDirUpString();
}

bool SceneEditor::SetCurrentCameraPosDirUp(const std::string & val)
{
    return m_cameraController.setFromPosDirUpString(val);
}

void SceneEditor::SetCameraVerticalFOV(float cameraFOV)
{
    m_cameraController.setVerticalFOV(cameraFOV);
}

void SceneEditor::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    m_cameraController.setIntrinsics(fx, fy, cx, cy, width, height);
}

void SceneEditor::ClearCameraIntrinsics()
{
    m_cameraController.clearIntrinsics();
}

void SceneEditor::SaveCurrentCamera() const
{
    m_cameraController.saveToFile();
}

void SceneEditor::LoadCurrentCamera()
{
    m_cameraController.loadFromFile();
}

void SceneEditor::RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    GetRayTracingResources().requestMeshAccelRebuild(mesh);
}

void SceneEditor::SetEnvMapOverrideSource(const std::string& envMapOverride)
{
    GetLightingPasses().setEnvMapOverrideSource(envMapOverride);
}















bool SceneEditor::ShouldRenderUnfocused() const
{
    if (GetWorldRenderer()->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork() )
    {
        // Make sure we at least run one render frame to allow expensive resource creation to happen in background, and to allow at least somewhat decent convergence so when user alt-tabs they get a nice image
        return true;
    }

    if (m_editor.RenderWhenOutOfFocus)
    {
        return true;
    }

    // Let Reference mode accumulate all frames before pausing
    return (!m_settings.RealtimeMode && (GetWorldRenderer()->getAccumulationSampleIndex() < m_settings.AccumulationTarget));
}

void SceneEditor::SetSceneTime( double sceneTime ) 
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

std::shared_ptr<caustica::Material> SceneEditor::FindMaterial(int materialID) const
{
    return SceneManager::findMaterial(m_sceneManager->getScene(), materialID);
}

caustica::ecs::Entity SceneEditor::FindEntityByInstanceIndex(int instanceIndex) const
{
    return SceneManager::findEntityByInstanceIndex(m_sceneManager->getScene(), instanceIndex);
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
        m_zoomTool = std::make_unique<ZoomTool>(GetDevice(), m_shaderFactory);
    return m_zoomTool.get();
}

static float ReadR11G11B10FloatChannel(uint32_t packed, uint32_t channel)
{
    uint16_t halfBits = 0;
    switch (channel)
    {
    case 0: halfBits = uint16_t((packed << 4) & 0x7FF0); break;
    case 1: halfBits = uint16_t((packed >> 7) & 0x7FF0); break;
    default: halfBits = uint16_t((packed >> 17) & 0x7FE0); break;
    }

    const float value = Float16ToFloat32(float16_t{ halfBits });
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

static nvrhi::TextureDesc MakeReadbackTextureDesc(nvrhi::TextureDesc desc, const char* debugName)
{
    desc.debugName = debugName;
    desc.isRenderTarget = false;
    desc.isUAV = false;
    desc.isTypeless = false;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    return desc;
}

static void ReadR11G11B10Float3Staging(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture, uint32_t width, uint32_t height, std::vector<float>& output)
{
    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
        return;

    output.resize(size_t(width) * size_t(height) * 3);
    for (uint32_t y = 0; y < height; y++)
    {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            const uint32_t packed = row[x];
            output[targetOffset + 0] = ReadR11G11B10FloatChannel(packed, 0);
            output[targetOffset + 1] = ReadR11G11B10FloatChannel(packed, 1);
            output[targetOffset + 2] = ReadR11G11B10FloatChannel(packed, 2);
        }
    }

    device->unmapStagingTexture(stagingTexture);
}

static void ReadRGBA16Float3Staging(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture, uint32_t width, uint32_t height, std::vector<float>& output)
{
    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
        return;

    output.resize(size_t(width) * size_t(height) * 3);
    for (uint32_t y = 0; y < height; y++)
    {
        const float16_t4* row = reinterpret_cast<const float16_t4*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const float4 value = Float16ToFloat32x4(row[x]);
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            output[targetOffset + 0] = std::isfinite(value.x) ? std::clamp(value.x, -1.0f, 1.0f) : 0.0f;
            output[targetOffset + 1] = std::isfinite(value.y) ? std::clamp(value.y, -1.0f, 1.0f) : 0.0f;
            output[targetOffset + 2] = std::isfinite(value.z) ? std::clamp(value.z, -1.0f, 1.0f) : 1.0f;
        }
    }

    device->unmapStagingTexture(stagingTexture);
}

nvrhi::ITexture* SceneEditor::GetLdrColorTexture() const
{
    const auto* targets = GetWorldRenderer() ? GetWorldRenderer()->getRenderTargets() : nullptr;
    return targets ? targets->LdrColor.Get() : nullptr;
}

const DebugFeedbackStruct& SceneEditor::GetFeedbackData() const
{
    static const DebugFeedbackStruct kEmpty{};
    return GetWorldRenderer() ? GetWorldRenderer()->getFeedbackData() : kEmpty;
}

const DeltaTreeVizPathVertex* SceneEditor::GetDebugDeltaPathTree() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getDebugDeltaPathTree() : nullptr;
}

int SceneEditor::GetAccumulationSampleIndex() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getAccumulationSampleIndex() : 0;
}

uint2 SceneEditor::GetRenderSize() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getRenderSize() : uint2{0, 0};
}

uint2 SceneEditor::GetDisplaySize() const
{
    return GetWorldRenderer() ? GetWorldRenderer()->getDisplaySize() : uint2{0, 0};
}

bool SceneEditor::AccumulationCompleted() const
{
    return GetWorldRenderer() && GetWorldRenderer()->getAccumulationCompleted();
}

void SceneEditor::recordFrameTiming(const caustica::GpuDevice& gpuDevice)
{
    double frameTime = gpuDevice.GetAverageFrameTimeSeconds();
    if (frameTime <= 0.0 && m_lastDeltaTime > 0.0f)
        frameTime = static_cast<double>(m_lastDeltaTime);
    UpdateFpsInfo(frameTime);
}

void SceneEditor::UpdateFpsInfo(double frameTimeSeconds)
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

void SceneEditor::afterWorldRender(caustica::GpuDevice& gpuDevice)
{
    auto* worldRenderer = GetWorldRenderer();
    if (!worldRenderer)
        return;

    if (m_settings.ContinuousDebugFeedback || m_renderState.Picking.hasActivePickRequest())
    {
        ResolvePickFeedback(worldRenderer->getFeedbackData());
        m_renderState.Picking.clearPickRequests();
    }

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (!framebuffer)
            return false;
        nvrhi::ITexture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        return SaveTextureToFile(
            gpuDevice.GetDevice(), GetCommonPasses().get(), texture, nvrhi::ResourceStates::Common, fileName);
    };
    CaptureScriptPostRender(saveFramebuffer);

    if (ConsumeExperimentalPhotoScreenshot())
    {
        nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(true);
        if (framebuffer)
            worldRenderer->denoisedScreenshot(framebuffer->getDesc().colorAttachments[0].texture);
    }
}

void SceneEditor::BackBufferResizing()
{
    if (auto* r = GetWorldRenderer())
        r->onBackBufferResizing();
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

// =============================================================================
// Input event handling
// =============================================================================

void SceneEditor::onEvent(caustica::Event& event)
{
    m_inputRouter.onEvent(event);
}

} // namespace caustica::editor
