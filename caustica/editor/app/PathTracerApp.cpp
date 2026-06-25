#include "PathTracerApp.h"
#include "input/PathTracerInputController.h"
#include "render/PathTracingRenderer.h"

#include <render/Core/PostProcessAA.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>

#include <core/path_utils.h>
#include <scene/scene_utils.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/BindingCache.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/Core/View.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/json.h>
#include <core/vfs/VFS.h>
#include <math/float.h>
#include <math/math.h>
#include <shaders/light_cb.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>
#include <rhi/common/misc.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <render/Core/PTPipelineBaker.h>
#include <render/Core/ComputePipelineBaker.h>

#include "render/Core/AccelerationStructureUtil.h"

#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>

#include <render/Passes/OMM/OmmBaker.h>

#include "SampleCommon/LocalConfig.h"
#include "SampleCommon/CaptureScriptManager.h"
#include <render/Passes/Debug/Korgi.h>

#include <render/GPUSort/GPUSort.h>

#include <render/Passes/Debug/ZoomTool.h>

#include <render/Passes/PostProcess/DenoisingGuidesBaker.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>

#include <assets/loader/GltfImporter.h>
#include <assets/loader/ObjImporter.h>
#include <scene/SceneGraph.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include "SampleGame/GameScene.h"

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

#include <fstream>
#include <iostream>

#include <thread>

namespace
{
    constexpr const char* c_InlineSceneSentinel = "__CAUSTICA_INLINE_SCENE_JSON__";

    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }
}

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

namespace
{
    constexpr float kGaussianSplatKernelMinResponse = 0.0113f;

    uint32_t ResolveGaussianSplatShadowMode(const PathTracerSettings& settings)
    {
        if (!settings.GaussianSplatShadows && settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : settings.GaussianSplatShadowsMode;
        return uint32_t(std::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT));
    }

    uint32_t ClampGaussianSplatSoftShadowSamples(int sampleCount)
    {
        return uint32_t(std::clamp(sampleCount, 1, 16));
    }

    uint32_t ClampGaussianSplatEmissionProxyCount(int proxyCount)
    {
        return uint32_t(std::clamp(proxyCount, 0, 262144));
    }

    std::string MakeUniqueChildNodeName(const SceneGraphNode& parent, const std::string& desiredName)
    {
        const std::string baseName = desiredName.empty() ? "GaussianSplat" : desiredName;

        std::unordered_set<std::string> existingNames;
        for (size_t childIndex = 0; childIndex < parent.GetNumChildren(); childIndex++)
            existingNames.insert(parent.GetChild(childIndex)->GetName());

        if (existingNames.find(baseName) == existingNames.end())
            return baseName;

        for (uint32_t suffix = 2; ; suffix++)
        {
            std::string candidate = baseName + " (" + std::to_string(suffix) + ")";
            if (existingNames.find(candidate) == existingNames.end())
                return candidate;
        }
    }

    bool NodeSubtreeContains(SceneGraphNode* root, SceneGraphNode* candidate)
    {
        if (root == nullptr || candidate == nullptr)
            return false;

        SceneGraphWalker walker(root);
        while (walker)
        {
            if (walker.Get() == candidate)
                return true;
            walker.Next(true);
        }

        return false;
    }

    bool IsGaussianSplatEmissionEnabled(const SampleUIData& ui)
    {
        return ui.EnableGaussianSplats
            && ui.GaussianSplatAsEmitter
            && ui.GaussianSplatEmissionIntensity > 0.0f
            && ui.GaussianSplatEmissionMaxProxyCount > 0;
    }

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

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
// Required for Agility SDK on Windows 10. Setup 1.c. 2.a.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = CAUSTICA_D3D_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

const char* g_windowTitle = "caustica";

const float c_envMapRadianceScale = 1.0f / 4.0f;

FPSLimiter g_FPSLimiter;

PathTracerApp::PathTracerApp(caustica::GpuDevice& deviceManager,
    const CommandLineOptions& cmdLine,
    SampleUIData& ui)
    : caustica::RenderContext(&deviceManager)
    , m_cmdLine(cmdLine)
    , m_settings(ui)
    , m_editor(ui)
    , m_ui(ui)
    , m_renderCore(deviceManager.GetDevice())
{
    m_progressLoading.Start("Initializing...");
    m_progressLoading.Set(50);

    m_renderCore.camera().camera().SetRotateSpeed(.003f);

#if CAUSTICA_WITH_STREAMLINE
    if (!GetGpuDevice()->GetDeviceParams().headlessDevice)
    {
        m_settings.IsDLSSSuported = GetGpuDevice()->GetStreamline().IsDLSSAvailable();
        m_settings.IsDLSSFGSupported = GetGpuDevice()->GetStreamline().IsDLSSGAvailable();
        m_settings.IsReflexSupported = GetGpuDevice()->GetStreamline().IsReflexAvailable();
        m_settings.IsDLSSRRSupported = GetGpuDevice()->GetStreamline().IsDLSSRRAvailable();
    }
#endif

    RefreshEnvironmentMapMediaList();

    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_ui, m_cmdLine);

#if CAUSTICA_WITH_PYTHON
    // Embedded Python scripting host - we always create the wrapper but the
    // interpreter itself is initialized on demand the first time a script
    // gets queued.  This keeps cold-start overhead at zero when scripting is
    // unused even if the executable was built with CAUSTICA_WITH_PYTHON=ON.
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

PathTracerApp::~PathTracerApp()
{
    if (m_inputController)
        GetGpuDevice()->UnregisterInputHandler(m_inputController.get());

#if CAUSTICA_WITH_PYTHON
    // Tear down the Python interpreter first so that any nb::class_<>-bound
    // C++ objects (materials, lights, ...) are released while their owning
    // C++ data is still alive.
    m_pythonScripting.reset();
#endif
}

void PathTracerApp::DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 )
{
    auto& lines = m_pathTracingRenderer->getCpuSideDebugLines();
    if (int(lines.size()) + 2 >= MAX_DEBUG_LINES) return;
    DebugLineStruct dls = { float4(start, 1), col1 };
    DebugLineStruct dle = { float4(stop, 1), col2 };
    lines.push_back(dls);
    lines.push_back(dle);
}

void PathTracerApp::Init(const std::string& preferredScene,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    m_shaderFactory = shaderFactory;

    m_CommonPasses = std::make_shared<caustica::CommonRenderPasses>(GetDevice(), m_shaderFactory);
    m_bindingCache = std::make_unique<caustica::BindingCache>(GetDevice());

    m_pathTracingRenderer = std::make_unique<PathTracingRenderer>(*this);
    m_pathTracingRenderer->createBindingLayouts();

    m_DescriptorTable = std::make_shared<caustica::DescriptorTableManager>(GetDevice(), m_pathTracingRenderer->getBindlessLayout());

    m_pathTracingRenderer->createDeviceResources();

    m_settings.EnableGaussianSplats = true;
    m_settings.GaussianSplatDepthTest = m_cmdLine.GaussianSplatDepthTest;
    m_settings.GaussianSplatScale = m_cmdLine.GaussianSplatScale;
    m_settings.GaussianSplatAlphaScale = m_cmdLine.GaussianSplatAlphaScale;
    m_settings.GaussianSplatBrightness = m_cmdLine.GaussianSplatBrightness;
    m_settings.GaussianSplatAsEmitter = m_cmdLine.GaussianSplatAsEmitter;
    m_settings.GaussianSplatEmissionIntensity = m_cmdLine.GaussianSplatEmissionIntensity;
    m_settings.GaussianSplatEmissionMaxProxyCount = m_cmdLine.GaussianSplatEmissionMaxProxyCount;
    m_settings.GaussianSplatAlphaCullThreshold = m_cmdLine.GaussianSplatAlphaCullThreshold;
    
    m_sampleGame = std::make_unique<GameScene>(*this, m_cmdLine);
    m_progressLoading.Set(95);

    auto nativeFS = std::make_shared<caustica::NativeFileSystem>();
    m_TextureCache = std::make_shared<caustica::TextureCache>(GetDevice(), nativeFS, m_DescriptorTable);

    m_sceneManager = std::make_unique<SceneManager>(
        *GetGpuDevice(), *m_shaderFactory, m_TextureCache, m_DescriptorTable);

    m_sceneManager->setLoadingCallbacks(
        [this]() { SceneLoaded(); },
        [this]() { SceneUnloading(); });

    m_renderCore.initializeRenderPipeline(m_shaderFactory);

    if (GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        m_ommBaker = std::make_shared<OmmBaker>(GetDevice(), m_DescriptorTable, m_TextureCache, m_shaderFactory);

    // Get all scenes in "assets" folder ??delegated to SceneManager
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

    m_inputController = std::make_unique<PathTracerInputController>(PathTracerInputController::Bindings{
        .gpuDevice = *GetGpuDevice(),
        .renderCore = m_renderCore,
        .settings = m_settings,
        .editor = m_editor,
        .sampleGame = m_sampleGame.get(),
        .getZoomTool = [this]() { return m_zoomTool.get(); },
        .getUpscalingScale = [this]() -> dm::float2 {
            if (m_pathTracingRenderer->getRenderTargets() == nullptr)
                return dm::float2(1.0f, 1.0f);
            return dm::float2(m_pathTracingRenderer->getRenderSize()) / dm::float2(m_pathTracingRenderer->getDisplaySize());
        },
    });
    GetGpuDevice()->RegisterInputHandler(m_inputController.get());
}

void PathTracerApp::SetCurrentScene( const std::string & sceneName, bool forceReload )
{
    if (!m_sceneManager->beginSceneSwitch(sceneName, GetLocalPath(c_AssetsFolder), forceReload))
        return;

    m_settings.ResetAccumulation = true;
    m_sceneManager->setAsyncLoadingEnabled(false);

    m_progressLoading.Stop();
    m_progressLoading.Start("Loading scene...");
    m_sceneManager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        m_sceneManager->getCurrentScenePath());
    if( m_sceneManager->getScene() == nullptr )
    {
        caustica::error( "Unable to load scene '%s'", sceneName.c_str() );
        m_sceneManager->clearScene();
        m_progressLoading.Stop();
        return;
    }
}

bool PathTracerApp::LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return AttachGaussianSplatToScene(fileName, convertRdfToRub);
}

std::filesystem::path
PathTracerApp::ResolveGaussianSplatPath(const GaussianSplat& splat) const
{
    if (splat.path.empty())
        return {};

    std::filesystem::path splatPath = splat.path;
    if (splatPath.is_absolute())
        return splatPath;

    const std::filesystem::path sceneFolder = m_sceneManager->getCurrentScenePath().parent_path();
    if (!sceneFolder.empty() && m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        return sceneFolder / splatPath;

    return std::filesystem::absolute(splatPath);
}

void PathTracerApp::PrepareGaussianSplatPass(GaussianSplatPass& pass)
{
    auto* renderTargets = m_pathTracingRenderer->getRenderTargets();
    auto shaderDebug = m_pathTracingRenderer->getShaderDebug();
    if (renderTargets == nullptr || shaderDebug == nullptr)
        return;

    if (m_gpuSort == nullptr)
        m_gpuSort = std::make_shared<GPUSort>(GetDevice(), m_shaderFactory);
    m_gpuSort->CreateRenderPasses(m_CommonPasses, shaderDebug);
    pass.SetGpuSort(m_gpuSort);
    pass.CreatePipeline(*renderTargets);
}

uint32_t
PathTracerApp::GetTotalGaussianSplatCount() const
{
    uint64_t total = 0;
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.pass != nullptr)
            total += object.pass->GetSplatCount();
    }
    return uint32_t(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

void PathTracerApp::UpdateGaussianSplatUIState()
{
    m_editor.GaussianSplatObjectCount = uint32_t(m_gaussianSplatSceneObjects.size());
    m_editor.GaussianSplatCount = GetTotalGaussianSplatCount();

    m_gaussianSplatFileNameSummary.clear();
    if (m_gaussianSplatSceneObjects.size() == 1 && m_gaussianSplatSceneObjects.front().pass != nullptr)
    {
        m_gaussianSplatFileNameSummary = m_gaussianSplatSceneObjects.front().pass->GetSourceFileName();
    }
    else if (!m_gaussianSplatSceneObjects.empty())
    {
        m_gaussianSplatFileNameSummary = std::to_string(m_gaussianSplatSceneObjects.size()) + " scene Gaussian Splat objects";
    }
    m_editor.GaussianSplatFileName = m_gaussianSplatFileNameSummary;

    if (m_editor.GaussianSplatObjectCount == 0)
        m_editor.SelectedGaussianSplat = false;
}

void PathTracerApp::LoadGaussianSplatsFromScene()
{
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();

    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_shaderFactory)
    {
        UpdateGaussianSplatUIState();
        return;
    }

    SceneGraphWalker walker(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode().get());
    while (walker)
    {
        auto splat = std::dynamic_pointer_cast<GaussianSplat>(walker->GetLeaf());
        if (splat != nullptr)
        {
            splat->loadedSplatCount = 0;
            splat->resolvedPath.clear();

            const std::filesystem::path splatPath = ResolveGaussianSplatPath(*splat);
            if (splatPath.empty())
            {
                caustica::error("Gaussian Splat node '%s' has no path/file field.", walker->GetName().c_str());
            }
            else
            {
                auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
                if (pass->LoadFromFile(splatPath, splat->convertRdfToRub))
                {
                    splat->resolvedPath = splatPath.string();
                    splat->loadedSplatCount = pass->GetSplatCount();
                    PrepareGaussianSplatPass(*pass);

                    GaussianSplatSceneObject object;
                    object.splat = splat;
                    object.node = walker->shared_from_this();
                    object.pass = std::move(pass);
                    m_gaussianSplatSceneObjects.push_back(std::move(object));
                }
                else
                {
                    caustica::error("Failed to load Gaussian Splat node '%s' from '%s'.",
                        walker->GetName().c_str(), splatPath.string().c_str());
                }
            }
        }

        walker.Next(true);
    }

    UpdateGaussianSplatUIState();
    m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
}

bool PathTracerApp::AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_sceneManager->getScene()->GetSceneGraph()->GetRootNode())
    {
        caustica::error("Cannot load Gaussian splats before a scene is loaded.");
        return false;
    }
    if (!m_shaderFactory)
    {
        caustica::error("Cannot load Gaussian splats before the shader factory is initialized.");
        return false;
    }

    std::filesystem::path splatPath = fileName;
    if (!splatPath.is_absolute())
        splatPath = std::filesystem::absolute(splatPath);

    if (!std::filesystem::exists(splatPath))
    {
        caustica::error("Gaussian Splat file does not exist: '%s'", splatPath.string().c_str());
        return false;
    }

    auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
    if (!pass->LoadFromFile(splatPath, convertRdfToRub))
    {
        caustica::error("Failed to load Gaussian Splat file '%s'.", splatPath.string().c_str());
        return false;
    }
    if (pass->GetSplatCount() == 0)
    {
        caustica::error("Gaussian Splat file '%s' contains no splats.", splatPath.string().c_str());
        return false;
    }

    auto splat = std::make_shared<GaussianSplat>();
    splat->path = splatPath.string();
    splat->resolvedPath = splatPath.string();
    splat->convertRdfToRub = convertRdfToRub;
    splat->enabled = true;
    splat->loadedSplatCount = pass->GetSplatCount();

    auto node = std::make_shared<SceneGraphNode>();
    auto sceneGraph = m_sceneManager->getScene()->GetSceneGraph();
    auto rootNode = sceneGraph->GetRootNode();
    node->SetName(MakeUniqueChildNodeName(*rootNode, splatPath.filename().string()));
    node->SetLeaf(splat);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    node->SetTranslation(dm::double3(
        double(m_settings.GaussianSplatTranslation.x),
        double(m_settings.GaussianSplatTranslation.y),
        double(m_settings.GaussianSplatTranslation.z)));
    node->SetRotation(dm::rotationQuat(dm::double3(
        double(m_settings.GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(m_settings.GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(m_settings.GaussianSplatRotationEulerDeg.z) * deg2rad)));
    node->SetScaling(dm::double3(
        double(m_settings.GaussianSplatObjectScale.x),
        double(m_settings.GaussianSplatObjectScale.y),
        double(m_settings.GaussianSplatObjectScale.z)));

    auto attachedNode = sceneGraph->Attach(rootNode, node);
    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    PrepareGaussianSplatPass(*pass);

    GaussianSplatSceneObject object;
    object.splat = splat;
    object.node = attachedNode;
    object.pass = std::move(pass);
    m_gaussianSplatSceneObjects.push_back(std::move(object));

    m_settings.EnableGaussianSplats = true;
    UpdateGaussianSplatUIState();
    m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
    RequestFullRebuild();
    return true;
}

uint32_t
PathTracerApp::GetGaussianSplatCount() const
{
    return GetTotalGaussianSplatCount();
}

uint32_t
PathTracerApp::GetGaussianSplatObjectCount() const
{
    return uint32_t(m_gaussianSplatSceneObjects.size());
}

const std::string&
PathTracerApp::GetGaussianSplatFileName() const
{
    return m_gaussianSplatFileNameSummary;
}

PathTracerApp::GaussianSplatSceneObject*
PathTracerApp::GetPrimaryGaussianSplatObject()
{
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

const PathTracerApp::GaussianSplatSceneObject*
PathTracerApp::GetPrimaryGaussianSplatObject() const
{
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

float4x4
PathTracerApp::GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const
{
    auto node = object.node.lock();
    if (node == nullptr)
        return float4x4::identity();

    return dm::affineToHomogeneous(node->GetLocalToWorldTransformFloat());
}

void PathTracerApp::BuildGaussianSplatEmissionProxyList()
{
    m_gaussianSplatEmissionProxies.clear();

    if (!IsGaussianSplatEmissionEnabled(m_ui))
        return;

    const uint32_t maxProxyCount = ClampGaussianSplatEmissionProxyCount(m_settings.GaussianSplatEmissionMaxProxyCount);
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > m_gaussianSplatEmissionProxies.size()
            ? maxProxyCount - uint32_t(m_gaussianSplatEmissionProxies.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        object.pass->BuildEmissionProxies(
            remainingProxyCount,
            m_settings.GaussianSplatScale,
            uint32_t(std::clamp(m_settings.GaussianSplatRtxKernelDegree, 0, 5)),
            m_settings.GaussianSplatRtxAdaptiveClamp,
            m_settings.GaussianSplatTintColor,
            m_settings.GaussianSplatAlphaCullThreshold);

        auto node = object.node.lock();
        const dm::affine3 objectToWorld = node != nullptr
            ? node->GetLocalToWorldTransformFloat()
            : dm::affine3::identity();

        const float radiusScale = std::max({
            length(objectToWorld.transformVector(float3(1.0f, 0.0f, 0.0f))),
            length(objectToWorld.transformVector(float3(0.0f, 1.0f, 0.0f))),
            length(objectToWorld.transformVector(float3(0.0f, 0.0f, 1.0f))) });

        const auto& proxies = object.pass->GetEmissionProxies();
        m_gaussianSplatEmissionProxies.reserve(m_gaussianSplatEmissionProxies.size() + proxies.size());
        for (const GaussianSplatEmissionProxy& proxy : proxies)
        {
            GaussianSplatEmissionProxy transformed = proxy;
            transformed.center = objectToWorld.transformPoint(proxy.center);
            transformed.radius = proxy.radius * radiusScale;
            m_gaussianSplatEmissionProxies.push_back(transformed);
        }
    }
}

void PathTracerApp::SceneUnloading( )
{
    m_editor.TogglableNodes = nullptr;
    if (m_pathTracingRenderer)
        m_pathTracingRenderer->onSceneUnloading();
    m_renderCore.onSceneUnloading();
    m_bindingCache->Clear( );
    m_lights.clear();
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedNode = nullptr;
    m_editor.SelectedGaussianSplat = false;
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();
    UpdateGaussianSplatUIState();
    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapBaker = nullptr;
    m_lightsBaker = nullptr;
    m_materialsBaker = nullptr;
    m_gpuSort = nullptr;
    m_uncompressedTextures.clear();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneUnloading();

    DestroyRTPipelines();
    m_computePipelineBaker = nullptr;

    if (m_sampleGame!=nullptr) m_sampleGame->SceneUnloading();
}

bool PathTracerApp::IsSceneLoading() const
{
    return m_sceneManager && m_sceneManager->isSceneLoading();
}

bool PathTracerApp::IsSceneLoaded() const
{
    return m_sceneManager && m_sceneManager->isSceneLoaded();
}

void PathTracerApp::UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera )
{
    m_renderCore.camera().updateFromSceneCamera(sceneCamera);

    auto sceneCameraEx = std::dynamic_pointer_cast<caustica::PerspectiveCamera>(sceneCamera);
    if (sceneCameraEx != nullptr)
    {
        ToneMappingParameters defaults;
        m_settings.ToneMappingParams.autoExposure =
            sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
        m_settings.ToneMappingParams.exposureCompensation =
            sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
        m_settings.ToneMappingParams.exposureValue =
            sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
        m_settings.ToneMappingParams.exposureValueMin =
            sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
        m_settings.ToneMappingParams.exposureValueMax =
            sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
    }
}

void PathTracerApp::UpdateViews( nvrhi::IFramebuffer* framebuffer )
{
    (void)framebuffer;
    m_renderCore.camera().updateViews(makeCameraUpdateParams());
}

caustica::CameraUpdateParams PathTracerApp::makeCameraUpdateParams() const
{
    auto& r = *m_pathTracingRenderer;
    caustica::CameraUpdateParams params;
    params.renderSize = r.getRenderSize();
    params.displayAspectRatio = r.getDisplayAspectRatio();
    params.sampleIndex = r.getSampleIndex();
    params.frameIndex = r.getFrameIndex();
    params.realtimeMode = m_settings.RealtimeMode;
    params.realtimeAA = m_settings.RealtimeAA;
    params.dbgFreezeRealtimeNoiseSeed = m_settings.DbgFreezeRealtimeNoiseSeed;
    params.temporalAAJitter = m_settings.TemporalAntiAliasingJitter;
    params.temporalAAPass = r.getTemporalAntiAliasingPass();
    return params;
}

void PathTracerApp::CollectUncompressedTextures()
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
    for ( auto textureIT : m_materialsBaker->GetUsedTextures() )
        listUncompressedTextureIfNeeded(textureIT.second.Loaded, textureIT.second.NormalMap);
}

void PathTracerApp::RefreshEnvironmentMapMediaList()
{
    const std::filesystem::path currentScenePath = m_sceneManager
        ? m_sceneManager->getCurrentScenePath()
        : std::filesystem::path();

    SceneManager::refreshEnvironmentMapMediaList(
        GetLocalPath(c_AssetsFolder),
        c_EnvMapSubFolder,
        currentScenePath,
        m_envMapMediaList,
        m_envMapMediaFolder);
}

void PathTracerApp::SceneLoaded( )
{
    if (m_pathTracingRenderer)
        m_pathTracingRenderer->resetFrameIndex();

    RefreshEnvironmentMapMediaList();

    m_progressLoading.Set(50);

    if (m_sampleGame != nullptr) m_sampleGame->SceneLoaded(m_sceneManager->getScene(), m_sceneManager->getCurrentScenePath(), GetLocalPath(c_AssetsFolder));

    m_progressLoading.Set(55);

    if (m_TextureCache && m_CommonPasses)
    {
        m_TextureCache->ProcessRenderingThreadCommands(*m_CommonPasses, 0.f);
        m_TextureCache->LoadingFinished();
    }

    m_progressLoading.Set(60);

    m_sceneTime = 0.f;
    m_sceneManager->getScene()->FinishedLoading( GetFrameIndex( ) );

    LoadGaussianSplatsFromScene();

    if (!m_initialGaussianSplatAttached && !m_cmdLine.GaussianSplatFileName.empty())
    {
        m_initialGaussianSplatAttached = true;
        (void)AttachGaussianSplatToScene(m_cmdLine.GaussianSplatFileName, m_cmdLine.GaussianSplatConvertRdfToRub);
    }

    m_progressLoading.Set(65);

	// Find lights; do this before special cases to avoid duplicates
	for (auto light : m_sceneManager->getScene()->GetSceneGraph()->GetLights())
	{
		m_lights.push_back(light);
	}

    // seem like sensible defaults
    m_settings.ToneMappingParams.exposureCompensation = 2.0f;
    m_settings.ToneMappingParams.exposureValue = 0.0f;

    std::shared_ptr<EnvironmentLight> envLight = FindEnvironmentLight(m_lights);
    m_envMapLocalPath = (envLight==nullptr)?(""):(envLight->path);
    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;

    if (m_editor.TogglableNodes == nullptr)
    {
        m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*m_editor.TogglableNodes, GetScene()->GetSceneGraph()->GetRootNode().get()); // UNSAFE - make sure not to keep m_editor.TogglableNodes longer than scenegraph!
    }

    // clean up invisible lights / markers because they slow things down
    for (int i = (int)m_lights.size() - 1; i >= 0; i--)
    {
        LightConstants lc;
        m_lights[i]->FillLightConstants(lc);
        if (length(lc.color * lc.intensity) <= 1e-7f)
            m_lights.erase( m_lights.begin() + i );
    }

    if( m_envMapLocalPath != "" )
    {
        // Make sure that there's an environment light object attached to the scene,
        // so that RTXDI will pick it up and sample.
        if (envLight == nullptr)
        {
            envLight = std::make_shared<EnvironmentLight>();
            m_sceneManager->getScene()->GetSceneGraph()->AttachLeafNode(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), envLight);
            m_lights.push_back(envLight);
        }
    }

    auto cameras = m_sceneManager->getScene()->GetSceneGraph( )->GetCameras( );
    auto camScene = (cameras.empty( ))?(nullptr):(std::dynamic_pointer_cast<PerspectiveCamera>(cameras.back()));
    if (camScene)
        UpdateCameraFromScene(camScene);
    else
        m_renderCore.camera().setupDefaultCamera();

    m_renderCore.onSceneLoaded(*m_sceneManager->getScene(), m_editor.AccelerationStructRebuildRequested);

    // PrintSceneGraph( m_sceneManager->getScene()->GetSceneGraph( )->GetRootNode( ) );

    m_editor.ShaderReloadRequested = true;  // we have to re-create shader hit table
    m_settings.EnableAnimations = false;
    m_settings.RealtimeMode = false;

    std::shared_ptr<SampleSettings> settings = m_sceneManager->getScene()->GetSampleSettingsNode();
    if (settings != nullptr)
    {
        m_settings.RealtimeMode = settings->realtimeMode.value_or(m_settings.RealtimeMode);
        m_settings.EnableAnimations = settings->enableAnimations.value_or(m_settings.EnableAnimations);
        if (settings->startingCamera.has_value())
            m_renderCore.camera().setSelectedCameraIndex(settings->startingCamera.value()+1); // slot 0 reserved for free flight camera
        if (settings->realtimeFireflyFilter.has_value())
        {
            m_settings.RealtimeFireflyFilterThreshold = settings->realtimeFireflyFilter.value();
            m_settings.RealtimeFireflyFilterEnabled = true;
        }
        m_settings.BounceCount = settings->maxBounces.value_or(m_settings.BounceCount);
        m_settings.DiffuseBounceCount = settings->maxDiffuseBounces.value_or(m_settings.DiffuseBounceCount);
        m_settings.TexLODBias = settings->textureMIPBias.value_or(m_settings.TexLODBias);
    }

    if (m_cmdLine.stopAnimations)
        m_settings.EnableAnimations = false;

    m_progressLoading.Set(70);

    LocalConfig::PostSceneLoad(*this, m_ui);

    m_progressLoading.Set(90);

    if (m_materialsBaker!=nullptr) m_materialsBaker->SceneReloaded();
    if (m_envMapBaker!=nullptr) m_envMapBaker->SceneReloaded();
    if (m_lightsBaker!=nullptr) m_lightsBaker->SceneReloaded();
    if (m_ommBaker!=nullptr) m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    m_progressLoading.Set(100);

    if (m_cmdLine.OverrideToRealtimeMode)
        m_settings.RealtimeMode = true;
    if (m_cmdLine.OverrideToReferenceMode)
        m_settings.RealtimeMode = false;
    
    if (m_cmdLine.OverrideAutoexposureOff)
    {
        m_settings.ToneMappingParams.autoExposure = false;
        m_settings.ToneMappingParams.exposureValue = 0.0f;
    }
    if (m_cmdLine.OverrideExposureOffset != FLT_MAX)
        m_settings.ToneMappingParams.exposureCompensation = m_cmdLine.OverrideExposureOffset;
    if (m_cmdLine.DisableFireflyFilters)
    {
        m_settings.RealtimeFireflyFilterEnabled = false;
        m_settings.ReferenceFireflyFilterEnabled = false;
    }
    if (m_cmdLine.DisablePostProcessFilters)
    {
        m_settings.EnableBloom = false;
    }
    if (m_cmdLine.cameraPosDirUp != "")
        SetCurrentCameraPosDirUp(m_cmdLine.cameraPosDirUp);

    m_settings.MaterialVariantIndex = 0;

    m_asyncLoadingInProgress = true;

#if CAUSTICA_WITH_PYTHON
    // Initialize the embedded Python interpreter (lazily) and queue the
    // command-line scripts/expressions so that they execute against a fully
    // populated scene.  Actual execution happens during Animate() below.
    if (m_pythonScripting && (!m_cmdLine.pythonScript.empty() || !m_cmdLine.pythonExpr.empty()))
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

void PathTracerApp::Animate(float fElapsedTimeSeconds)
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

    m_renderCore.camera().camera().SetMoveSpeed(m_settings.CameraMoveSpeed);

    if( m_editor.ShaderAndACRefreshDelayedRequest > 0 )
    {
        m_editor.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (m_editor.ShaderAndACRefreshDelayedRequest <= 0 )
        {
            m_editor.ShaderAndACRefreshDelayedRequest = 0;
            m_editor.ShaderReloadRequested = true;
            m_editor.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = m_settings.EnableAnimations && m_settings.RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || m_settings.ResetAccumulation;

    if (m_sampleGame) m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);

    if (auto* toneMappingPass = m_pathTracingRenderer->getToneMappingPass())
        toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

    if (IsSceneLoaded() && enableAnimationUpdate)
    {
        if (enableAnimations)
            m_sceneTime += fElapsedTimeSeconds;
        if (m_sampleGame && m_sampleGame->IsInitialized())
            m_sceneTime = m_sampleGame->GetGameTime();

        for (const auto& anim : m_sceneManager->getScene()->GetSceneGraph()->GetAnimations())
        {
            double cutLeft = 0.0; double cutRight = 0.0;
            // if (anim->GetName() == "Take 001") // special hack for mesh drone anim - TODO: fix properly in future
            // { cutLeft = 0.333; cutRight = 0.0; }
            anim->Apply((float)fmod(m_sceneTime+cutLeft, anim->GetDuration()-cutLeft-cutRight));
        }
    }
    else
    {
        m_sceneTime = 0.0f;
    }

    m_renderCore.camera().selectedCameraIndex() = std::min( m_renderCore.camera().selectedCameraIndex(), GetSceneCameraCount()-1 );
    if (m_renderCore.camera().selectedCameraIndex() > 0)
    {
        std::shared_ptr<caustica::PerspectiveCamera> sceneCamera = std::dynamic_pointer_cast<PerspectiveCamera>(m_sceneManager->getScene()->GetSceneGraph()->GetCameras()[m_renderCore.camera().selectedCameraIndex()-1]);
        if (sceneCamera != nullptr)
            UpdateCameraFromScene( sceneCamera );
    }

    m_renderCore.camera().camera().Animate(fElapsedTimeSeconds);

    if (m_sampleGame) m_sampleGame->TickCamera(fElapsedTimeSeconds, m_renderCore.camera().camera());

    if (m_settings.CameraAntiRRSleepJitter>0)
    {
        float off = 0.05f * ((m_pathTracingRenderer->getFrameIndex()%2)?(-m_settings.CameraAntiRRSleepJitter):(m_settings.CameraAntiRRSleepJitter));

        float3 dir = m_renderCore.camera().camera().GetDir();
        float3 right = normalize(cross(dir, m_renderCore.camera().camera().GetUp()));
        affine3 rot = rotation(right, off);
        dir = rot.transformVector(dir);

        m_renderCore.camera().camera().LookTo( m_renderCore.camera().camera().GetPosition(), dir, m_renderCore.camera().camera().GetUp() );
    }

    if (m_renderCore.camera().cameraMovedSinceLastFrame())
    {
        m_renderCore.camera().updateLastCameraState();
        if( !m_settings.RealtimeMode )
            m_settings.ResetAccumulation = true;
        m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
    }

    m_captureScriptManager->PostAnim();


    double frameTime = GetGpuDevice()->GetAverageFrameTimeSeconds();
    if (frameTime > 0.0)
    {
#if CAUSTICA_WITH_STREAMLINE
        if (m_settings.DLSSFGMultiplier != 1)
            m_fpsInfo = StringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G", frameTime * 1e3, m_settings.DLSSFGMultiplier, m_settings.DLSSFGMultiplier / frameTime);
        else
#endif
            m_fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);
    }

    // Window title
    std::string extraInfo = ", " + m_fpsInfo + ", " + m_sceneManager->getCurrentSceneName() + ", " + GetResolutionInfo() + ", (L: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetLights().size()) + ", MAT: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMaterials().size())
        + ", MESH: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMeshes().size()) + ", I: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMeshInstances().size()) + ", SI: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetSkinnedMeshInstances().size())
        //+ ", AvgLum: " + std::to_string((m_renderTargets!=nullptr)?(m_renderTargets->AvgLuminanceLastCaptured):(0.0f))
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";


    GetGpuDevice()->SetInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
}

std::string PathTracerApp::GetResolutionInfo() const
{
    auto& r = *m_pathTracingRenderer;
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

float PathTracerApp::GetAvgTimePerFrame() const
{
    if (m_benchFrames == 0) return 0.0f;
    std::chrono::duration<double> elapsed = (m_benchLast - m_benchStart);
    return float(elapsed.count() / m_benchFrames);
}

std::string PathTracerApp::GetCurrentCameraPosDirUp() const
{
    return m_renderCore.camera().getPosDirUpString();
}

bool PathTracerApp::SetCurrentCameraPosDirUp(const std::string & val)
{
    return m_renderCore.camera().setFromPosDirUpString(val);
}

void PathTracerApp::SetCameraVerticalFOV(float cameraFOV)
{
    m_renderCore.camera().setVerticalFOV(cameraFOV);
    m_settings.ResetAccumulation = true;
    m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
}

void PathTracerApp::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    m_renderCore.camera().setIntrinsics(fx, fy, cx, cy, width, height);
    m_settings.ResetAccumulation = true;
    m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
}

void PathTracerApp::ClearCameraIntrinsics()
{
    m_renderCore.camera().clearIntrinsics();
    m_settings.ResetAccumulation = true;
    m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
}

void PathTracerApp::SaveCurrentCamera() const
{
    dm::float4x4 projMatrix = m_renderCore.camera().view()->GetProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    m_renderCore.camera().saveToFile(
        GetDirectoryWithExecutable() / "campos.txt",
        m_renderCore.camera().zNear(),
        fovY);
}

void PathTracerApp::LoadCurrentCamera()
{
    m_renderCore.camera().loadFromFile(GetDirectoryWithExecutable() / "campos.txt");
}

void PathTracerApp::FillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro> & macros)
{
    macros.clear();

    auto device = GetDevice();
    const bool canUseNvapiHitObject =
        m_settings.NVAPIHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 &&
        device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV);
    const bool canUseDxHitObject =
        m_settings.DXHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;

    assert(!canUseNvapiHitObject || !canUseDxHitObject);

    macros.push_back({ "ENABLE_DEBUG_SURFACE_VIZ",  (m_settings.DebugView != DebugViewType::Disabled)?("1"):("0") });
    macros.push_back({ "ENABLE_DEBUG_LINES_VIZ",    (m_settings.ShowDebugLines)?("1"):("0") });

    macros.push_back({ "USE_NVAPI_HIT_OBJECT_EXTENSION", canUseNvapiHitObject ? "1" : "0" });
    macros.push_back({ "USE_NVAPI_REORDER_THREADS", (canUseNvapiHitObject && m_settings.NVAPIReorderThreads) ? "1" : "0" });

    macros.push_back({ "USE_DX_HIT_OBJECT_EXTENSION", canUseDxHitObject ? "1" : "0" });
    macros.push_back({ "USE_DX_MAYBE_REORDER_THREADS", (canUseDxHitObject && m_settings.DXMaybeReorderThreads) ? "1" : "0" });

    macros.push_back({ "PT_ENABLE_RUSSIAN_ROULETTE", (m_settings.EnableRussianRoulette) ? ("1") : ("0") });

    macros.push_back({ "PT_NEE_ENABLED", (m_settings.UseNEE)?("1"):("0") });

    macros.push_back({ "PT_USE_RESTIR_DI", (m_settings.ActualUseReSTIRDI()) ? ("1") : ("0") });   // these will match constants.useReSTIRDI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_GI", (m_settings.ActualUseReSTIRGI()) ? ("1") : ("0") });   // these will match constants.useReSTIRGI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_PT", (m_settings.ActualUseReSTIRPT()) ? ("1") : ("0") });
    

    // minor perf gains but recompile time every time value changed is too annoying 
    // macros.push_back({ "PT_BOUNCE_COUNT", std::to_string(m_settings.BounceCount) });
    // macros.push_back({ "PT_DIFFUSE_BOUNCE_COUNT", std::to_string((m_settings.RealtimeMode) ? (m_settings.RealtimeDiffuseBounceCount) : (m_settings.ReferenceDiffuseBounceCount)) });

    macros.push_back({ "CAUSTICA_USE_APPROXIMATE_MIS", (m_settings.ActualUseApproximateMIS()) ? ("1") : ("0") });

    // It helps performance a lot when these are baked in
    macros.push_back({ "CAUSTICA_NEE_FULL_SAMPLE_COUNT", std::to_string(m_settings.NEEFullSamples) });
    uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
    uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
    macros.push_back({ "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(m_settings.NEECandidateSamples) });

    macros.push_back({ "CAUSTICA_DISABLE_SER_TERMINATION_HINT", (m_settings.DbgDisableSERTerminationHint)?("1"):("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NON_NEE_LIGHTING", (m_settings.DbgDiscardNonNEELighting) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NEE_LIGHTING", (m_settings.DbgDiscardNEELighting) ? ("1") : ("0") });
    
    macros.push_back({ "CAUSTICA_FIREFLY_FILTER", (m_settings.ActualFireflyFilterEnabled()) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", std::to_string(m_settings.StablePlanesActiveCount) });

    macros.push_back({ "CAUSTICA_NESTED_DIELECTRICS_QUALITY", std::to_string(m_settings.NestedDielectricsQuality) });

    macros.push_back({ "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", (m_settings.UseFp16Types) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", (m_settings.EnableLDSamplerForBSDF) ? ("1") : ("0") });

    m_lightsBaker->SetGlobalShaderMacros(macros);
    if (m_ommBaker != nullptr)
        m_ommBaker->SetGlobalShaderMacros(macros);
}

extern HitGroupInfo ComputeSubInstanceHitGroupInfo(const PTMaterial& material);

bool PathTracerApp::CreatePTPipeline(caustica::ShaderFactory& /*shaderFactory*/)
{
    return m_pathTracingRenderer->createPTPipeline();
}

void PathTracerApp::CreateBlases(nvrhi::ICommandList* commandList)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings.AS.ExcludeTransmissive };
    m_renderCore.accelStructs().createBlases(commandList, *m_sceneManager->getScene(), settings);
}

void PathTracerApp::UploadSubInstanceData(nvrhi::ICommandList* commandList)
{
    m_renderCore.accelStructs().uploadSubInstanceData(commandList);
}

void PathTracerApp::CreateTlas(nvrhi::ICommandList* commandList)
{
    m_renderCore.accelStructs().createTlas(commandList, *m_sceneManager->getScene());
}

void PathTracerApp::CreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if(m_ommBaker) m_ommBaker->CreateOpacityMicromaps(*m_sceneManager->getScene());
    CreateBlases(commandList);
    CreateTlas(commandList);
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        if (ResolveGaussianSplatShadowMode(m_settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            object.pass->BuildAccelerationStructures(
                commandList,
                m_settings.GaussianSplatUseAABBs,
                m_settings.GaussianSplatUseTLASInstances,
                m_settings.GaussianSplatBlasCompaction,
                m_settings.GaussianSplatScale,
                uint32_t(std::clamp(m_settings.GaussianSplatRtxKernelDegree, 0, 5)),
                m_settings.GaussianSplatRtxAdaptiveClamp);
        else
            object.pass->ReleaseAccelerationStructures();
    }
}

void PathTracerApp::RecreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if (m_editor.AccelerationStructRebuildRequested)
    {
        m_editor.AccelerationStructRebuildRequested = false;
        m_settings.ResetAccumulation = true;

        GetDevice()->waitForIdle();

        m_pathTracingRenderer->invalidateBindingSet();
        m_renderCore.accelStructs().releaseGpuResources();

        m_renderCore.accelStructs().clearMeshAccelStructs(*m_sceneManager->getScene());
        GetDevice()->runGarbageCollection();

        commandList->open();
        CreateAccelStructs(commandList);
        commandList->close();
        GetDevice()->executeCommandList(commandList);
        GetDevice()->waitForIdle();
    }
}

void PathTracerApp::RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    m_settings.ResetAccumulation = true;

    if (!m_renderCore.accelStructs().hasTopLevelAS())
    {
        m_editor.AccelerationStructRebuildRequested = true;
        return;
    }

    m_renderCore.accelStructs().requestMeshRebuild(mesh);
}

void PathTracerApp::BackBufferResizing()
{
    m_pathTracingRenderer->onBackBufferResizing();
}




void PathTracerApp::SetEnvMapOverrideSource(const std::string& envMapOverride) 
{ 
    if (m_envMapOverride != envMapOverride && m_envMapBaker != nullptr)
        m_envMapBaker->SetTargetCubeResolution(0);  // reset resolution just to avoid getting crazy with procedural sky as it's very slow
    m_envMapOverride = envMapOverride; 
}















bool PathTracerApp::ShouldRenderUnfocused()
{
    if (m_pathTracingRenderer->getFrameIndex() < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork() )
    {
        // Make sure we at least run one render frame to allow expensive resource creation to happen in background, and to allow at least somewhat decent convergence so when user alt-tabs they get a nice image
        return true;
    }

    if (m_editor.RenderWhenOutOfFocus)
    {
        return true;
    }

    // Let Reference mode accumulate all frames before pausing
    return (!m_settings.RealtimeMode && (m_pathTracingRenderer->getAccumulationSampleIndex() < m_settings.AccumulationTarget));
}

void PathTracerApp::SetSceneTime( double sceneTime ) 
{ 
    if (m_sampleGame && m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_sceneTime = sceneTime; 
}

double PathTracerApp::GetSceneTime()
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        return m_sampleGame->GetGameTime();
    return m_sceneTime;
}


void PathTracerApp::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (m_sceneManager->getScene() == nullptr)
    {
        assert(false);
        return;
    }
    m_progressLoading.Stop();
    m_asyncLoadingInProgress = false;
    HandleDroppedFiles();
    m_pathTracingRenderer->render(framebuffer);
}


void PathTracerApp::RecreateBindingSet()
{
    m_pathTracingRenderer->recreateBindingSet();
}


std::shared_ptr<caustica::Material> PathTracerApp::FindMaterial(int materialID) const
{
    return SceneManager::findMaterial(m_sceneManager->getScene(), materialID);
}

std::shared_ptr<caustica::SceneGraphNode> PathTracerApp::FindNodeByInstanceIndex(int instanceIndex) const
{
    return SceneManager::findNodeByInstanceIndex(m_sceneManager->getScene(), instanceIndex);
}


void PathTracerApp::HandleDroppedFiles()
{
    if (m_editor.PendingDroppedFiles.empty())
        return;

    auto files = std::move(m_editor.PendingDroppedFiles);
    m_editor.PendingDroppedFiles.clear();

    for (const auto& filePath : files)
    {
        std::filesystem::path path(filePath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        if (ext == ".ply")
        {
            caustica::info("Drag-drop: loading Gaussian Splat file '%s'", filePath.c_str());
            if (LoadGaussianSplatFile(path))
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects", GetGaussianSplatCount(), GetGaussianSplatObjectCount());
            else
                caustica::error("Failed to load Gaussian Splat file '%s'", filePath.c_str());
        }
        else if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
        {
            caustica::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (LoadMeshFile(path))
                caustica::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                caustica::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            caustica::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj)", ext.c_str());
        }
    }
}

bool PathTracerApp::LoadMeshFile(const std::filesystem::path& filePath)
{
    if (!m_sceneManager->getScene() || !m_shaderFactory || !m_TextureCache)
    {
        caustica::error("Cannot load mesh: scene, shader factory, or texture cache not initialized.");
        return false;
    }

    std::filesystem::path absPath = filePath;
    if (!absPath.is_absolute())
        absPath = std::filesystem::absolute(absPath);

    if (!std::filesystem::exists(absPath))
    {
        caustica::error("File does not exist: '%s'", absPath.string().c_str());
        return false;
    }

    std::string ext = absPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (ext == ".gltf" || ext == ".glb")
        return LoadGltfMeshFile(absPath);
    if (ext == ".obj")
        return LoadObjMeshFile(absPath);

    caustica::error("Unsupported mesh file type '%s'.", ext.c_str());
    return false;
}

bool PathTracerApp::LoadGltfMeshFile(const std::filesystem::path& filePath)
{
    auto fs = std::make_shared<caustica::NativeFileSystem>();
    auto sceneTypeFactory = std::make_shared<caustica::render::RenderSceneTypeFactory>();
    auto importer = std::make_shared<caustica::GltfImporter>(fs, sceneTypeFactory);

    caustica::SceneLoadingStats stats;
    caustica::SceneImportResult importResult;

    std::filesystem::path sceneDirectory;
    if (m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_sceneManager->getCurrentScenePath().parent_path();

    if (!importer->Load(filePath, *m_TextureCache, stats, nullptr, importResult, sceneDirectory))
    {
        caustica::error("GltfImporter failed to load '%s'", filePath.string().c_str());
        return false;
    }

    if (!importResult.rootNode)
    {
        caustica::error("GltfImporter produced no root node for '%s'", filePath.string().c_str());
        return false;
    }

    importResult.rootNode->SetName(filePath.stem().string());

    auto importedRoot = m_sceneManager->getScene()->GetSceneGraph()->Attach(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}


bool PathTracerApp::LoadObjMeshFile(const std::filesystem::path& filePath)
{
    auto sceneTypeFactory = std::make_shared<caustica::render::RenderSceneTypeFactory>();
    caustica::ObjImporter importer(sceneTypeFactory);

    caustica::SceneLoadingStats stats;
    SceneImportResult importResult;
    if (!importer.Load(filePath, *m_TextureCache, stats, nullptr, importResult))
        return false;

    if (!importResult.rootNode)
        return false;

    auto importedRoot = m_sceneManager->getScene()->GetSceneGraph()->Attach(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}

void PathTracerApp::FinalizeRuntimeSceneMutation(const std::shared_ptr<caustica::SceneGraphNode>& importedRoot)
{
    if (importedRoot)
    {
        std::unordered_set<caustica::Material*> processedMaterials;
        SceneGraphWalker walker(importedRoot.get());
        while (walker)
        {
            auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(walker->GetLeaf());
            if (meshInstance && meshInstance->GetMesh())
            {
                for (const auto& geometry : meshInstance->GetMesh()->geometries)
                {
                    if (geometry->material && processedMaterials.insert(geometry->material.get()).second)
                        LocalConfig::PostMaterialLoad(*geometry->material);
                }
            }
            walker.Next(true);
        }
    }

    m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());

    if (m_materialsBaker != nullptr)
        m_materialsBaker->SceneReloaded();
    if (m_lightsBaker != nullptr)
        m_lightsBaker->SceneReloaded();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    RequestFullRebuild();
}

bool PathTracerApp::DeleteSceneNode(const std::shared_ptr<SceneGraphNode>& node)
{
    if (node == nullptr || m_sceneManager->getScene() == nullptr)
        return false;

    auto sceneGraph = m_sceneManager->getScene()->GetSceneGraph();
    if (sceneGraph == nullptr)
        return false;

    auto rootNode = sceneGraph->GetRootNode();
    if (rootNode == nullptr || node == rootNode)
        return false;

    if (node->GetGraph() != sceneGraph || node->GetParent() == nullptr)
        return false;

    GetDevice()->waitForIdle();

    bool removedGaussianSplat = false;
    auto removedSplatBegin = std::remove_if(
        m_gaussianSplatSceneObjects.begin(),
        m_gaussianSplatSceneObjects.end(),
        [&node, &removedGaussianSplat](const GaussianSplatSceneObject& object)
        {
            auto objectNode = object.node.lock();
            const bool remove = objectNode != nullptr && NodeSubtreeContains(node.get(), objectNode.get());
            removedGaussianSplat = removedGaussianSplat || remove;
            return remove;
        });
    if (removedSplatBegin != m_gaussianSplatSceneObjects.end())
        m_gaussianSplatSceneObjects.erase(removedSplatBegin, m_gaussianSplatSceneObjects.end());

    sceneGraph->Detach(node, true);
    m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());

    m_lights.clear();
    for (auto light : sceneGraph->GetLights())
        m_lights.push_back(light);

    if (m_editor.TogglableNodes != nullptr)
    {
        m_editor.TogglableNodes->clear();
        UpdateTogglableNodes(*m_editor.TogglableNodes, rootNode.get());
    }

    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedNode = nullptr;
    m_editor.InspectorRotationNode.reset();
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

    if (removedGaussianSplat)
    {
        UpdateGaussianSplatUIState();
        m_gaussianSplatEmissionProxies.clear();
        m_pathTracingRenderer->setGaussianSplatTemporalReset(true);
    }

    if (m_materialsBaker != nullptr)
        m_materialsBaker->SceneReloaded();
    if (m_lightsBaker != nullptr)
        m_lightsBaker->SceneReloaded();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    RequestFullRebuild();
    return true;
}


void PathTracerApp::RequestFullRebuild()
{
    m_editor.AccelerationStructRebuildRequested = true;
    m_editor.ShaderReloadRequested = true;
    m_editor.ShaderAndACRefreshDelayedRequest = 0.0f;
    m_settings.ResetAccumulation = true;
    m_pathTracingRenderer->invalidateBindingSet();
    if (m_bindingCache)
        m_bindingCache->Clear();
}

struct UniquePositionMapForDeform
{
    std::vector<float3> uniquePositions;
    std::vector<uint32_t> renderToUnique;
};

static std::array<uint32_t, 3> PositionKeyForDeform(const float3& p)
{
    std::array<uint32_t, 3> key{};
    std::memcpy(&key[0], &p.x, sizeof(uint32_t));
    std::memcpy(&key[1], &p.y, sizeof(uint32_t));
    std::memcpy(&key[2], &p.z, sizeof(uint32_t));
    return key;
}

static UniquePositionMapForDeform BuildUniquePositionMapForDeform(
    const std::vector<float3>& renderVertices,
    const std::vector<uint32_t>* sourcePositionIndices = nullptr)
{
    if (sourcePositionIndices && sourcePositionIndices->size() == renderVertices.size())
    {
        UniquePositionMapForDeform result;
        result.renderToUnique.resize(renderVertices.size());

        std::vector<size_t> renderOrder;
        renderOrder.reserve(renderVertices.size());
        for (size_t i = 0; i < renderVertices.size(); ++i)
            renderOrder.push_back(i);

        std::sort(renderOrder.begin(), renderOrder.end(),
            [&](size_t a, size_t b)
            {
                const uint32_t sourceA = (*sourcePositionIndices)[a];
                const uint32_t sourceB = (*sourcePositionIndices)[b];
                return sourceA == sourceB ? a < b : sourceA < sourceB;
            });

        std::unordered_map<uint32_t, uint32_t> uniqueLookup;
        uniqueLookup.reserve(renderVertices.size());

        for (size_t renderIndex : renderOrder)
        {
            const uint32_t sourceIndex = (*sourcePositionIndices)[renderIndex];
            auto found = uniqueLookup.find(sourceIndex);
            if (found == uniqueLookup.end())
            {
                const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
                uniqueLookup.emplace(sourceIndex, uniqueIndex);
                result.uniquePositions.push_back(renderVertices[renderIndex]);
                result.renderToUnique[renderIndex] = uniqueIndex;
            }
            else
            {
                result.renderToUnique[renderIndex] = found->second;
            }
        }

        return result;
    }

    struct KeyHash
    {
        size_t operator()(const std::array<uint32_t, 3>& key) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(key[0]);
            h ^= std::hash<uint32_t>{}(key[1]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(key[2]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    UniquePositionMapForDeform result;
    result.renderToUnique.reserve(renderVertices.size());

    std::unordered_map<std::array<uint32_t, 3>, uint32_t, KeyHash> uniqueLookup;
    uniqueLookup.reserve(renderVertices.size());

    for (const float3& vertex : renderVertices)
    {
        const auto key = PositionKeyForDeform(vertex);
        auto found = uniqueLookup.find(key);
        if (found == uniqueLookup.end())
        {
            const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
            uniqueLookup.emplace(key, uniqueIndex);
            result.uniquePositions.push_back(vertex);
            result.renderToUnique.push_back(uniqueIndex);
        }
        else
        {
            result.renderToUnique.push_back(found->second);
        }
    }

    return result;
}

static std::vector<float3> GetMeshRenderVerticesForDeform(
    const std::shared_ptr<MeshInfo>& mesh,
    const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error(std::string(caller) + ": mesh has no buffer group");
    if (mesh->totalVertices == 0)
        return {};

    const auto& positions = mesh->buffers->positionData;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + size_t(mesh->totalVertices);
    if (positions.size() < end)
        throw std::runtime_error(std::string(caller) + ": CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    return std::vector<float3>(positions.begin() + begin, positions.begin() + end);
}

static const std::vector<uint32_t>* GetMeshSourcePositionIndicesForDeform(
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount)
{
    auto meshEx = std::dynamic_pointer_cast<MeshInfoEx>(mesh);
    if (!meshEx || meshEx->DeformationSourcePositionIndices.size() != renderVertexCount)
        return nullptr;

    return &meshEx->DeformationSourcePositionIndices;
}

std::vector<float3> PathTracerApp::GetMeshVertices(const std::shared_ptr<MeshInfo>& mesh) const
{
    std::vector<float3> renderVertices = GetMeshRenderVerticesForDeform(mesh, "GetMeshVertices");
    return BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size())).uniquePositions;
}

static std::shared_ptr<MeshInfo> GetMeshFromSceneNodeForDeform(
    const std::shared_ptr<SceneGraphNode>& node,
    const char* caller)
{
    if (!node)
        throw std::runtime_error(std::string(caller) + ": node is null");

    auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(node->GetLeaf());
    if (!meshInstance || !meshInstance->GetMesh())
        throw std::runtime_error(std::string(caller) + ": node is not a mesh instance");

    return meshInstance->GetMesh();
}

static std::shared_ptr<SceneGraphNode> FindUniqueMeshInstanceNodeForDeform(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!scene || !scene->GetSceneGraph())
        throw std::runtime_error(std::string(caller) + ": no scene is loaded");

    std::shared_ptr<SceneGraphNode> result;
    size_t instanceCount = 0;

    for (const auto& instance : scene->GetSceneGraph()->GetMeshInstances())
    {
        if (!instance || instance->GetMesh() != mesh)
            continue;

        if (auto node = instance->GetNodeSharedPtr())
        {
            result = node;
            ++instanceCount;
        }
    }

    if (instanceCount == 0)
        throw std::runtime_error(std::string(caller) + ": mesh has no scene instance");
    if (instanceCount > 1)
        throw std::runtime_error(std::string(caller) + ": mesh has multiple scene instances; pass a SceneNode instead");

    return result;
}

std::vector<float3> PathTracerApp::GetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_sceneManager->getScene(), mesh, "GetMeshVerticesWorld");
    return GetMeshVerticesWorld(node);
}

std::vector<float3> PathTracerApp::GetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node)
{
    if (!m_sceneManager->getScene())
        throw std::runtime_error("GetMeshVerticesWorld: no scene is loaded");

    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "GetMeshVerticesWorld");
    std::vector<float3> vertices = GetMeshVertices(mesh);

    const affine3 localToWorld = node->GetLocalToWorldTransformFloat();
    for (float3& vertex : vertices)
        vertex = localToWorld.transformPoint(vertex);

    return vertices;
}

void PathTracerApp::SetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_sceneManager->getScene(), mesh, "SetMeshVerticesWorld");
    SetMeshVerticesWorld(node, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void PathTracerApp::SetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    if (!m_sceneManager->getScene())
        throw std::runtime_error("SetMeshVerticesWorld: no scene is loaded");

    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "SetMeshVerticesWorld");
    const affine3 worldToLocal = affine3(inverse(node->GetLocalToWorldTransform()));

    std::vector<float3> objectVertices;
    objectVertices.reserve(vertices.size());
    for (const float3& vertex : vertices)
    {
        const float3 objectVertex = worldToLocal.transformPoint(vertex);
        if (!dm::all(dm::isfinite(objectVertex)))
            throw std::runtime_error("SetMeshVerticesWorld: world-to-object transform produced a non-finite vertex");
        objectVertices.push_back(objectVertex);
    }

    SetMeshVertices(mesh, objectVertices, recomputeNormals, rebuildAccelerationStructure);
}

static float3 NormalizeOrFallbackForDeform(const float3& v, const float3& fallback)
{
    const float len2 = dot(v, v);
    if (len2 <= 1e-20f || !std::isfinite(len2))
        return fallback;
    return v * (1.0f / std::sqrt(len2));
}

static void RecomputeMeshNormalsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);

    if (buffers.normalData.size() < vertexEnd || buffers.indexData.empty())
        return;

    std::vector<float3> renderVertices(buffers.positionData.begin() + vertexBegin, buffers.positionData.begin() + vertexEnd);
    UniquePositionMapForDeform uniqueMap = BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size()));

    std::vector<float3> accumulated(uniqueMap.uniquePositions.size(), float3(0.0f));

    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry || geometry->type != MeshGeometryPrimitiveType::Triangles)
            continue;

        const size_t indexBegin = size_t(mesh->indexOffset) + size_t(geometry->indexOffsetInMesh);
        const size_t indexEnd = indexBegin + size_t(geometry->numIndices);
        if (buffers.indexData.size() < indexEnd)
            continue;

        for (size_t i = indexBegin; i + 2 < indexEnd; i += 3)
        {
            uint32_t local[3] = {
                buffers.indexData[i + 0],
                buffers.indexData[i + 1],
                buffers.indexData[i + 2]
            };

            const uint32_t maxIndex = std::max(local[0], std::max(local[1], local[2]));
            if (maxIndex < geometry->numVertices)
            {
                local[0] += geometry->vertexOffsetInMesh;
                local[1] += geometry->vertexOffsetInMesh;
                local[2] += geometry->vertexOffsetInMesh;
            }

            if (local[0] >= mesh->totalVertices || local[1] >= mesh->totalVertices || local[2] >= mesh->totalVertices)
                continue;

            const float3& p0 = buffers.positionData[vertexBegin + local[0]];
            const float3& p1 = buffers.positionData[vertexBegin + local[1]];
            const float3& p2 = buffers.positionData[vertexBegin + local[2]];
            const float3 faceNormal = cross(p1 - p0, p2 - p0);
            if (!std::isfinite(dot(faceNormal, faceNormal)))
                continue;

            accumulated[uniqueMap.renderToUnique[local[0]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[1]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[2]]] += faceNormal;
        }
    }

    for (uint32_t i = 0; i < mesh->totalVertices; ++i)
    {
        const float3 normal = NormalizeOrFallbackForDeform(accumulated[uniqueMap.renderToUnique[i]], float3(0.0f, 1.0f, 0.0f));
        buffers.normalData[vertexBegin + i] = vectorToSnorm8(normal);
    }
}

static void UpdateMeshBoundsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);
    if (buffers.positionData.size() < vertexEnd)
        return;

    mesh->objectSpaceBounds = box3::empty();
    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry)
            continue;

        box3 bounds = box3::empty();
        const size_t geomBegin = vertexBegin + size_t(geometry->vertexOffsetInMesh);
        const size_t geomEnd = std::min(vertexEnd, geomBegin + size_t(geometry->numVertices));
        for (size_t i = geomBegin; i < geomEnd; ++i)
            bounds |= buffers.positionData[i];

        geometry->objectSpaceBounds = bounds;
        mesh->objectSpaceBounds |= bounds;
    }
}

static bool BufferRangeContainsBytes(const nvrhi::BufferRange& range, uint64_t relativeOffset, uint64_t byteSize)
{
    return range.byteSize != 0 && relativeOffset <= range.byteSize && byteSize <= range.byteSize - relativeOffset;
}

static nvrhi::ResourceStates GetMeshVertexBufferReadyState(const nvrhi::BufferDesc& desc)
{
    nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
    if (desc.isAccelStructBuildInput)
        state = state | nvrhi::ResourceStates::AccelStructBuildInput;
    return state;
}

static bool UploadMeshDeformationToGpu(
    nvrhi::IDevice* device,
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount,
    const std::vector<float3>* previousRenderVertices,
    bool uploadNormals)
{
    if (!device || !mesh || !mesh->buffers)
        return false;

    auto& buffers = *mesh->buffers;
    if (!buffers.vertexBuffer)
        return false;

    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertexCount;
    if (buffers.positionData.size() < end)
        return false;

    const nvrhi::BufferRange& positionRange = buffers.getVertexBufferRange(VertexAttribute::Position);
    const uint64_t positionOffset = uint64_t(begin) * sizeof(float3);
    const uint64_t positionBytes = uint64_t(renderVertexCount) * sizeof(float3);
    if (!BufferRangeContainsBytes(positionRange, positionOffset, positionBytes))
        return false;

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(
        buffers.vertexBuffer,
        buffers.positionData.data() + begin,
        positionBytes,
        positionRange.byteOffset + positionOffset);

    if (previousRenderVertices && previousRenderVertices->size() == renderVertexCount)
    {
        const nvrhi::BufferRange& prevPositionRange = buffers.getVertexBufferRange(VertexAttribute::PrevPosition);
        if (BufferRangeContainsBytes(prevPositionRange, positionOffset, positionBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                previousRenderVertices->data(),
                positionBytes,
                prevPositionRange.byteOffset + positionOffset);
        }
    }

    if (uploadNormals && buffers.normalData.size() >= end)
    {
        const nvrhi::BufferRange& normalRange = buffers.getVertexBufferRange(VertexAttribute::Normal);
        const uint64_t normalOffset = uint64_t(begin) * sizeof(uint32_t);
        const uint64_t normalBytes = uint64_t(renderVertexCount) * sizeof(uint32_t);
        if (BufferRangeContainsBytes(normalRange, normalOffset, normalBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                buffers.normalData.data() + begin,
                normalBytes,
                normalRange.byteOffset + normalOffset);
        }
    }

    commandList->setBufferState(buffers.vertexBuffer, GetMeshVertexBufferReadyState(buffers.vertexBuffer->getDesc()));
    commandList->close();
    device->executeCommandList(commandList);
    return true;
}

void PathTracerApp::SetMeshVertices(const std::shared_ptr<MeshInfo>& mesh,
                             const std::vector<float3>& vertices,
                             bool recomputeNormals,
                             bool rebuildAccelerationStructure)
{
    if (!mesh)
        throw std::runtime_error("SetMeshVertices: mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error("SetMeshVertices: mesh has no buffer group");

    std::vector<float3> renderVertices = GetMeshRenderVerticesForDeform(mesh, "SetMeshVertices");
    UniquePositionMapForDeform uniqueMap = BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size()));
    if (vertices.size() != uniqueMap.uniquePositions.size())
    {
        throw std::runtime_error(
            "SetMeshVertices: vertex count must match get_mesh_vertices(...) length");
    }

    for (size_t i = 0; i < renderVertices.size(); ++i)
        renderVertices[i] = vertices[uniqueMap.renderToUnique[i]];

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertices.size();
    if (buffers.positionData.size() < end)
        throw std::runtime_error("SetMeshVertices: CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    const std::vector<float3> previousRenderVertices(buffers.positionData.begin() + begin, buffers.positionData.begin() + end);
    std::copy(renderVertices.begin(), renderVertices.end(), buffers.positionData.begin() + begin);
    UpdateMeshBoundsFromPositions(mesh);

    if (recomputeNormals)
        RecomputeMeshNormalsFromPositions(mesh);

    const bool uploadedToExistingGpuBuffer = UploadMeshDeformationToGpu(
        GetDevice(),
        mesh,
        renderVertices.size(),
        &previousRenderVertices,
        recomputeNormals);

    if (!uploadedToExistingGpuBuffer)
    {
        GetDevice()->waitForIdle();

        if (buffers.vertexBuffer)
        {
            buffers.vertexBuffer = nullptr;
            buffers.vertexBufferDescriptor.reset();
            buffers.vertexBufferRanges.fill(nvrhi::BufferRange());
        }

        std::vector<std::shared_ptr<SceneGraphNode>> affectedNodes;
        if (m_sceneManager->getScene() && m_sceneManager->getScene()->GetSceneGraph())
        {
            for (const auto& instance : m_sceneManager->getScene()->GetSceneGraph()->GetMeshInstances())
            {
                if (instance && instance->GetMesh() == mesh)
                {
                    if (auto node = instance->GetNodeSharedPtr())
                        affectedNodes.push_back(node);
                }
            }
        }

        for (const auto& node : affectedNodes)
        {
            if (node && node->GetLeaf())
            {
                auto leaf = node->GetLeaf();
                node->SetLeaf(leaf);
            }
        }

        if (m_sceneManager->getScene())
            m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());
    }

    if (rebuildAccelerationStructure)
        RequestMeshAccelRebuild(mesh);
    else
        m_settings.ResetAccumulation = true;
}

void PathTracerApp::PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants)
{
    m_pathTracingRenderer->pathTrace(framebuffer, constants);
}


void PathTracerApp::Denoise(nvrhi::IFramebuffer* framebuffer)
{
    m_pathTracingRenderer->denoise(framebuffer);
}


#if CAUSTICA_WITH_NATIVE_DLSS

#endif

void PathTracerApp::PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    m_pathTracingRenderer->postProcessAA(framebuffer, reset);
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







dm::float2 PathTracerApp::ComputeCameraJitter(uint frameIndex)
{
    (void)frameIndex;
    return m_renderCore.camera().computeJitter(makeCameraUpdateParams());
}

nvrhi::ITexture* PathTracerApp::GetLdrColorTexture() const
{
    const auto* targets = m_pathTracingRenderer ? m_pathTracingRenderer->getRenderTargets() : nullptr;
    return targets ? targets->LdrColor.Get() : nullptr;
}

const DebugFeedbackStruct& PathTracerApp::GetFeedbackData() const
{
    return m_pathTracingRenderer->getFeedbackData();
}

const DeltaTreeVizPathVertex* PathTracerApp::GetDebugDeltaPathTree() const
{
    return m_pathTracingRenderer->getDebugDeltaPathTree();
}

int PathTracerApp::GetAccumulationSampleIndex() const
{
    return m_pathTracingRenderer ? m_pathTracingRenderer->getAccumulationSampleIndex() : 0;
}

uint2 PathTracerApp::GetRenderSize() const
{
    return m_pathTracingRenderer ? m_pathTracingRenderer->getRenderSize() : uint2{0, 0};
}

uint2 PathTracerApp::GetDisplaySize() const
{
    return m_pathTracingRenderer ? m_pathTracingRenderer->getDisplaySize() : uint2{0, 0};
}

bool PathTracerApp::AccumulationCompleted() const
{
    return m_pathTracingRenderer && m_pathTracingRenderer->getAccumulationCompleted();
}

void PathTracerApp::InvalidateBindingSet()
{
    if (m_pathTracingRenderer)
        m_pathTracingRenderer->invalidateBindingSet();
}

std::shared_ptr<PTPipelineBaker> PathTracerApp::GetRTPipelineBaker() const
{
    return m_pathTracingRenderer ? m_pathTracingRenderer->getPTPipelineBaker() : nullptr;
}

std::shared_ptr<PTPipelineVariant>& PathTracerApp::PtPipelineReference()
{
    return m_pathTracingRenderer->ptPipelineReference();
}

std::shared_ptr<PTPipelineVariant>& PathTracerApp::PtPipelineBuildStablePlanes()
{
    return m_pathTracingRenderer->ptPipelineBuildStablePlanes();
}

std::shared_ptr<PTPipelineVariant>& PathTracerApp::PtPipelineFillStablePlanes()
{
    return m_pathTracingRenderer->ptPipelineFillStablePlanes();
}

std::shared_ptr<PTPipelineVariant>& PathTracerApp::PtPipelineTestRaygenPPHDR()
{
    return m_pathTracingRenderer->ptPipelineTestRaygenPPHDR();
}

std::shared_ptr<PTPipelineVariant>& PathTracerApp::PtPipelineEdgeDetection()
{
    return m_pathTracingRenderer->ptPipelineEdgeDetection();
}
