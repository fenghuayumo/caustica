#include <engine/EngineApp.h>
#include <backend/GpuSurface.h>
#include <engine/internal/DefaultPlugins.h>
#include <engine/AppResources.h>
#include <engine/EntryPoint.h>
#include <engine/GpuSharedCaches.h>
#include <engine/LoadSession.h>
#include <engine/MeshDeformApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneStartup.h>
#include <assets/loader/TextureLoader.h>
#include <scene/SceneTypes.h>
#include <core/file_utils.h>
#include <core/json.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <platform/window.h>
#include <render/core/RenderDevice.h>
#include <rhi/rhi.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace caustica
{
namespace
{

std::filesystem::path GetCurrentModuleDirectory()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetCurrentModuleDirectory),
            &module))
    {
        std::array<wchar_t, 32768> path = {};
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size())
            return std::filesystem::path(path.data()).parent_path();
    }
#else
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&GetCurrentModuleDirectory), &info) && info.dli_fname)
        return std::filesystem::path(info.dli_fname).parent_path();
#endif
    return getDirectoryWithExecutable();
}

std::filesystem::path ResolveDefaultRuntimeDirectory()
{
    const std::filesystem::path moduleDirectory = GetCurrentModuleDirectory();
    if (std::filesystem::exists(moduleDirectory / "ShaderBin"))
        return moduleDirectory;

    const std::filesystem::path executableDirectory = getDirectoryWithExecutable();
    if (std::filesystem::exists(executableDirectory / "ShaderBin"))
        return executableDirectory;

    return moduleDirectory;
}

std::filesystem::path ResolveDefaultResourceRoot(const std::filesystem::path& runtimeDirectory)
{
    if (std::filesystem::exists(runtimeDirectory / c_AssetsFolder))
        return runtimeDirectory;

    const std::filesystem::path parentDirectory = runtimeDirectory.parent_path();
    if (std::filesystem::exists(parentDirectory / c_AssetsFolder))
        return parentDirectory;

    return getDirectoryWithExecutable();
}

caustica::rhi::GraphicsAPI ResolveGraphicsApi(const EngineAppDesc& desc)
{
#if CAUSTICA_WITH_DX12 && CAUSTICA_WITH_VULKAN
    return desc.useVulkan ? caustica::rhi::GraphicsAPI::VULKAN : caustica::rhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
    (void)desc;
    return caustica::rhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX12
    (void)desc;
    return caustica::rhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_DX11
    (void)desc;
    return caustica::rhi::GraphicsAPI::D3D11;
#else
#error "No graphics API enabled"
#endif
}

std::string TrimCopy(const std::string& value)
{
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
}

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    return value;
}

bool IsBuiltinModelReference(const std::string& modelName)
{
    return ToLowerCopy(modelName).rfind("builtin:", 0) == 0;
}

std::string NormalizeBuiltinModelName(std::string modelName)
{
    modelName = ToLowerCopy(TrimCopy(modelName));
    constexpr const char* prefix = "builtin:";
    if (modelName.rfind(prefix, 0) == 0)
        modelName.erase(0, std::strlen(prefix));

    for (char& ch : modelName)
    {
        if (ch == '-' || ch == ' ')
            ch = '_';
    }
    return modelName;
}

Json::Value MakeFloatArray(std::initializer_list<float> values)
{
    Json::Value array(Json::arrayValue);
    for (float value : values)
        array.append(value);
    return array;
}

} // namespace

std::string builtinSceneJson(const std::string& builtinModel)
{
    Json::Value root(Json::objectValue);
    root["entities"] = Json::Value(Json::arrayValue);

    Json::Value modelNode(Json::objectValue);
    modelNode["id"] = "DefaultBuiltinModel";
    modelNode["name"] = "DefaultBuiltinModel";
    modelNode["components"]["PrefabInstance"]["source"] =
        std::string("builtin:") + NormalizeBuiltinModelName(builtinModel);
    root["entities"].append(modelNode);

    Json::Value lights(Json::objectValue);
    lights["id"] = "Lights";
    lights["name"] = "Lights";
    root["entities"].append(lights);

    Json::Value sun(Json::objectValue);
    sun["id"] = "Sun";
    sun["name"] = "Sun";
    sun["parent"] = "Lights";
    sun["components"]["Transform"]["rotation"] =
        MakeFloatArray({ -0.23053891f, -0.15879166f, -0.6890466f, 0.6684697f });
    sun["components"]["DirectionalLight"]["angularSize"] = 1.5f;
    sun["components"]["DirectionalLight"]["color"] = MakeFloatArray({ 1.0f, 0.96f, 0.9f });
    sun["components"]["DirectionalLight"]["irradiance"] = 4.0f;
    root["entities"].append(sun);

    Json::Value fill(Json::objectValue);
    fill["id"] = "Fill";
    fill["name"] = "Fill";
    fill["parent"] = "Lights";
    fill["components"]["Transform"]["translation"] = MakeFloatArray({ 0.0f, 2.5f, 3.0f });
    fill["components"]["PointLight"]["color"] = MakeFloatArray({ 1.0f, 0.95f, 0.85f });
    fill["components"]["PointLight"]["intensity"] = 30.0f;
    fill["components"]["PointLight"]["radius"] = 0.05f;
    fill["components"]["PointLight"]["range"] = 10.0f;
    root["entities"].append(fill);

    Json::Value cameras(Json::objectValue);
    cameras["id"] = "Cameras";
    cameras["name"] = "Cameras";
    root["entities"].append(cameras);

    Json::Value camera(Json::objectValue);
    camera["id"] = "Default";
    camera["name"] = "Default";
    camera["parent"] = "Cameras";
    camera["components"]["Transform"]["translation"] = MakeFloatArray({ 0.0f, 1.15f, 5.0f });
    camera["components"]["Transform"]["rotation"] = MakeFloatArray({ 0.0f, 0.0f, 0.0f, 1.0f });
    camera["components"]["PerspectiveCameraEx"]["verticalFov"] = 0.7f;
    camera["components"]["PerspectiveCameraEx"]["zNear"] = 0.001f;
    camera["components"]["PerspectiveCameraEx"]["exposureCompensation"] = 1.0f;
    camera["components"]["PerspectiveCameraEx"]["enableAutoExposure"] = false;
    root["entities"].append(camera);

    root["settings"]["realtimeMode"] = true;
    return json::toString(root);
}

std::string prepareSceneSource(const std::string& scene)
{
    const std::string trimmed = TrimCopy(scene);
    if (trimmed.empty())
        return scene;
    if (IsBuiltinModelReference(trimmed))
        return builtinSceneJson(trimmed);
    return scene;
}

bool EngineAppDesc::applyCommandLine(const CommandLineOptions& options)
{
    cli = options;
    width = options.width;
    height = options.height;
    headless = options.noWindow;
    dedicatedRenderThread = !options.syncRender;
    parallelSystems = !options.serialSystems;
    debugDevice = options.debug;
    useVulkan = options.useVulkan;
    fullscreen = options.fullscreen;
    if (!options.scene.empty())
        scene = options.scene;
    if (!options.assetsDir.empty())
        assetPackRoot = options.assetsDir;

    std::string adapterError;
    if (!caustica::rhi::parseAdapterSelector(options.gpu, adapter, &adapterError))
    {
        caustica::error("Invalid --gpu selector '%s': %s", options.gpu.c_str(), adapterError.c_str());
        return false;
    }
    return true;
}

std::optional<EngineAppDesc> EngineAppDesc::fromArgv(int argc, char const* const* argv)
{
    CommandLineOptions options;
    if (!options.initFromCommandLine(argc, argv))
        return std::nullopt;

    EngineAppDesc desc;
    if (!desc.applyCommandLine(options))
        return std::nullopt;
    return desc;
}

std::unique_ptr<EngineApp> EngineApp::create(EngineAppDesc desc)
{
    auto engine = std::unique_ptr<EngineApp>(new EngineApp());
    if (!engine->initialize(std::move(desc)))
        return nullptr;
    return engine;
}

EngineApp::~EngineApp()
{
    shutdown();
}

bool EngineApp::initialize(EngineAppDesc desc)
{
    m_desc = std::move(desc);
    m_cmdLine = m_desc.cli;

    const std::filesystem::path runtimeDirectory = m_desc.runtimeDirectory.empty()
        ? ResolveDefaultRuntimeDirectory()
        : m_desc.runtimeDirectory;
    const std::filesystem::path resourceRoot = m_desc.resourceRoot.empty()
        ? ResolveDefaultResourceRoot(runtimeDirectory)
        : m_desc.resourceRoot;

    setRuntimeDirectoryOverride(runtimeDirectory);
    setLocalPathBaseOverride(resourceRoot);

    std::filesystem::path assetPackRoot = m_desc.assetPackRoot;
    if (assetPackRoot.empty() && !m_cmdLine.assetsDir.empty())
        assetPackRoot = m_cmdLine.assetsDir;
    if (assetPackRoot.empty())
        assetPackRoot = discoverAssetPackRoot(runtimeDirectory, resourceRoot);
    setAssetPackRootOverride(assetPackRoot);

    if (m_desc.device)
    {
        if (!m_desc.window && !m_desc.headless)
        {
            error("EngineApp: borrowed device requires a Window unless headless");
            return false;
        }
        if (!m_desc.surface)
        {
            error("EngineApp: borrowed device requires a GpuSurface");
            return false;
        }
        m_device = m_desc.device;
        m_window = m_desc.window;
        m_surface = m_desc.surface;
        m_ownsDevice = false;
    }
    else
    {
        if (m_desc.preGpuDeviceInit)
            m_desc.preGpuDeviceInit();

        GpuDeviceCreateDesc createDesc{};
        createDesc.api = ResolveGraphicsApi(m_desc);
        createDesc.headless = m_desc.headless;
        createDesc.windowTitle = m_desc.windowTitle;
        createDesc.backBufferWidth = m_desc.width;
        createDesc.backBufferHeight = m_desc.height;
        createDesc.adapter = m_desc.adapter;
        createDesc.enableDebug = m_desc.debugDevice;
        createDesc.startFullscreen = m_desc.fullscreen;
        createDesc.startMaximized = m_desc.maximized && !m_desc.fullscreen;
        if (m_desc.headless)
            createDesc.vsyncEnabled = false;
#if CAUSTICA_WITH_DX12
        createDesc.d3d12DeviceFactory = m_desc.d3d12DeviceFactory;
#endif

        if (!m_desc.headless)
        {
            m_ownedWindow = createGpuWindow(createDesc);
            if (!m_ownedWindow)
            {
                error("EngineApp: failed to create window");
                return false;
            }
            m_window = m_ownedWindow.get();
        }

        GpuDeviceCreateResult graphicsResult = GpuDevice::create(createDesc, m_window);
        if (!graphicsResult.gpuDevice || !graphicsResult.surface)
        {
            error("EngineApp: failed to create GPU device");
            return false;
        }

        m_ownedDevice = std::move(graphicsResult.gpuDevice);
        m_ownedSurface = std::move(graphicsResult.surface);
        m_device = m_ownedDevice.get();
        m_surface = m_ownedSurface.get();
        m_ownsDevice = true;
    }

    m_viewState.progressLoading.start("Starting up...");
    m_viewState.progressLoading.Set(50);

    const std::string preferredScene = m_desc.scene.empty()
        ? std::string("default.scene.json")
        : prepareSceneSource(m_desc.scene);

    m_app.reset(new App(m_device, m_desc.headless ? nullptr : m_window, m_surface));
    m_app->insertResourceRef(m_viewState);
    m_app->insertResourceRef(m_diagnostics);
    m_app->insertResourceRef(m_renderAppState);
    m_app->insertResourceRef(m_renderAppState.settings);
    m_app->insertResourceRef(m_renderAppState.runtime);
    m_app->insertResourceRef(m_cmdLine);
    m_app->emplaceResource<EngineBootstrap>(EngineBootstrap{
        .preferredScene = preferredScene,
        .hasSceneCallbacks = m_desc.hasSceneCallbacks,
        .sceneCallbacks = m_desc.sceneCallbacks,
    });
    m_app->addPlugins(DefaultPlugins{});
    m_app->setUseDedicatedRenderThread(m_desc.dedicatedRenderThread && !m_desc.headless);
    m_app->schedules().setParallelExecutionEnabled(m_desc.parallelSystems);

    m_valid = true;
    return true;
}

bool EngineApp::finishStartup()
{
    if (!m_valid || !m_app)
        return false;

    if (m_app->isStarted())
        return true;

    if (!m_app->finishStartup())
    {
        error("EngineApp: finishStartup failed");
        shutdown();
        return false;
    }

    return true;
}

void EngineApp::run()
{
    if (!m_valid || !m_app)
        return;
    if (!finishStartup())
        return;
    m_app->run();
    shutdown();
}

bool EngineApp::stepFrame(float dtSeconds)
{
    if (!m_valid || !m_app)
        return false;

    if (!finishStartup())
        return false;

    if (dtSeconds < 0.f)
        return m_app->stepFrame();

    return m_app->stepFrame(static_cast<double>(dtSeconds));
}

void EngineApp::requestExit()
{
    if (m_app)
        m_app->requestExit();
}

void EngineApp::shutdown()
{
    m_valid = false;

    m_app.reset();
    m_ownedSurface.reset();
    m_surface = nullptr;

    if (m_ownsDevice && m_ownedDevice)
    {
        m_ownedDevice->shutdown();
        m_ownedDevice.reset();
    }

    m_ownedWindow.reset();
    m_device = nullptr;
    m_window = nullptr;
    m_ownsDevice = false;
}

App& EngineApp::app()
{
    return *m_app;
}

const App& EngineApp::app() const
{
    return *m_app;
}

GpuDevice* EngineApp::device() const
{
    return m_device;
}

GpuSurface* EngineApp::surface() const
{
    return m_surface;
}

Window* EngineApp::window() const
{
    return m_window;
}

void EngineApp::setScene(const std::string& name, bool forceReload)
{
    if (m_app)
        caustica::setCurrentScene(*m_app, prepareSceneSource(name), forceReload);
}

bool EngineApp::isSceneLoaded() const
{
    return m_app && caustica::isSceneLoaded(*m_app);
}

bool EngineApp::isSceneLoading() const
{
    return m_app && caustica::isSceneLoading(*m_app);
}

bool EngineApp::isSceneReady() const
{
    if (!isSceneLoaded())
        return false;
    return !m_viewState.loadSession.isActive()
        && !m_viewState.sceneGpuSuspended.load(std::memory_order_acquire);
}

bool EngineApp::waitUntilReady(double timeoutSeconds, int warmupFrames)
{
    if (!m_valid || !m_app)
        return false;

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    while (timeoutSeconds <= 0.0 || std::chrono::steady_clock::now() - start < timeout)
    {
        if (!stepFrame(0.f))
            return false;
        if (isSceneReady())
        {
            const bool headless = m_window == nullptr;
            for (int i = 0; i < warmupFrames; ++i)
            {
                if (!stepFrame(headless ? (1.f / 60.f) : -1.f))
                    return false;
            }
            m_renderAppState.settings.ResetAccumulation = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    warning(
        "EngineApp: scene did not become ready within %.1f seconds (phase=%s gpuSuspended=%d)",
        timeoutSeconds,
        loadSessionPhaseName(m_viewState.loadSession.phase),
        m_viewState.sceneGpuSuspended.load(std::memory_order_acquire) ? 1 : 0);
    return false;
}

std::string EngineApp::currentSceneName() const
{
    return m_app ? caustica::currentSceneName(*m_app) : std::string{};
}

const std::vector<std::string>& EngineApp::availableScenes() const
{
    static const std::vector<std::string> kEmpty;
    return m_app ? caustica::availableScenes(*m_app) : kEmpty;
}

scene::SceneEntityWorld* EngineApp::entityWorld() const
{
    return m_app ? caustica::entityWorld(*m_app) : nullptr;
}

PathTracerSettings& EngineApp::settings()
{
    return m_app->resource<PathTracerSettings>();
}

const PathTracerSettings& EngineApp::settings() const
{
    return m_app->resource<PathTracerSettings>();
}

render::RenderAppState& EngineApp::renderAppState()
{
    return m_renderAppState;
}

const render::RenderAppState& EngineApp::renderAppState() const
{
    return m_renderAppState;
}

CommandLineOptions& EngineApp::commandLine()
{
    return m_cmdLine;
}

const CommandLineOptions& EngineApp::commandLine() const
{
    return m_cmdLine;
}

bool EngineApp::setCameraPosDirUp(const math::float3& pos, const math::float3& dir, const math::float3& up)
{
    return m_app && caustica::setCurrentCameraPosDirUp(*m_app, pos, dir, up);
}

bool EngineApp::setCameraPosDirUp(const std::string& value)
{
    return m_app && caustica::setCurrentCameraPosDirUp(*m_app, value);
}

std::string EngineApp::currentCameraPosDirUp() const
{
    return m_app ? caustica::currentCameraPosDirUp(*m_app) : std::string{};
}

void EngineApp::setCameraVerticalFOV(float radians)
{
    if (m_app)
        caustica::setCameraVerticalFOV(*m_app, radians);
}

float EngineApp::cameraVerticalFOV() const
{
    return m_app ? caustica::cameraVerticalFOV(*m_app) : 0.f;
}

void EngineApp::setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (m_app)
        caustica::setCameraIntrinsics(*m_app, fx, fy, cx, cy, width, height);
}

void EngineApp::clearCameraIntrinsics()
{
    if (m_app)
        caustica::clearCameraIntrinsics(*m_app);
}

uint32_t EngineApp::sceneCameraCount() const
{
    return m_app ? caustica::sceneCameraCount(*m_app) : 0;
}

uint32_t EngineApp::selectedCameraIndex() const
{
    if (!m_app)
        return 0;
    return caustica::selectedCameraIndex(*const_cast<App*>(m_app.get()));
}

void EngineApp::setSelectedCameraIndex(uint32_t index)
{
    if (m_app)
        caustica::selectedCameraIndex(*m_app) = index;
}

void EngineApp::saveCurrentCamera()
{
    if (m_app)
        caustica::saveCurrentCamera(*m_app);
}

void EngineApp::loadCurrentCamera()
{
    if (m_app)
        caustica::loadCurrentCamera(*m_app);
}

Handle<ScenePrefabAsset> EngineApp::load(const std::filesystem::path& path)
{
    return m_app ? caustica::load(*m_app, path) : Handle<ScenePrefabAsset>{};
}

ecs::Entity EngineApp::spawn(const Handle<ScenePrefabAsset>& prefab, const SceneApplyCallbacks& callbacks)
{
    return m_app ? caustica::spawn(*m_app, prefab, callbacks) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnFromFile(const std::filesystem::path& path, const SceneApplyCallbacks& callbacks)
{
    return m_app ? caustica::spawnFromFile(*m_app, path, callbacks) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnFromSource(const std::string& source, const SceneApplyCallbacks& callbacks)
{
    return m_app ? caustica::spawnFromSource(*m_app, source, callbacks) : ecs::NullEntity;
}

bool EngineApp::despawn(ecs::Entity entity)
{
    return m_app && caustica::despawn(*m_app, entity);
}

ecs::Entity EngineApp::spawnDirectionalLight(scene::DirectionalLightComponent component, const std::string& name)
{
    return m_app ? caustica::spawnDirectionalLight(*m_app, std::move(component), name) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnSpotLight(scene::SpotLightComponent component, const std::string& name)
{
    return m_app ? caustica::spawnSpotLight(*m_app, std::move(component), name) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnPointLight(scene::PointLightComponent component, const std::string& name)
{
    return m_app ? caustica::spawnPointLight(*m_app, std::move(component), name) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnRectLight(scene::RectLightComponent component, const std::string& name)
{
    return m_app ? caustica::spawnRectLight(*m_app, std::move(component), name) : ecs::NullEntity;
}

ecs::Entity EngineApp::spawnEnvironmentLight(scene::EnvironmentLightComponent component, const std::string& name)
{
    return m_app ? caustica::spawnEnvironmentLight(*m_app, std::move(component), name) : ecs::NullEntity;
}

bool EngineApp::loadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return m_app && caustica::loadGaussianSplatFile(*m_app, fileName, convertRdfToRub);
}

uint32_t EngineApp::gaussianSplatCount() const
{
    return m_app ? caustica::gaussianSplatCount(*m_app) : 0;
}

uint32_t EngineApp::gaussianSplatObjectCount() const
{
    return m_app ? caustica::gaussianSplatObjectCount(*m_app) : 0;
}

const std::string& EngineApp::gaussianSplatFileName() const
{
    static const std::string kEmpty;
    return m_app ? caustica::gaussianSplatFileName(*m_app) : kEmpty;
}

ecs::Entity EngineApp::findEntity(const std::filesystem::path& path, ecs::Entity context) const
{
    return m_app ? caustica::findEntity(*m_app, path, context) : ecs::NullEntity;
}

std::shared_ptr<Material> EngineApp::findMaterial(int materialID) const
{
    return m_app ? caustica::findMaterial(*m_app, materialID) : nullptr;
}

std::vector<dm::float3> EngineApp::getMeshVertices(ecs::Entity entity)
{
    return m_app ? caustica::getMeshVertices(*m_app, entity) : std::vector<dm::float3>{};
}

std::vector<dm::float3> EngineApp::getMeshVerticesWorld(ecs::Entity entity)
{
    return m_app ? caustica::getMeshVerticesWorld(*m_app, entity) : std::vector<dm::float3>{};
}

void EngineApp::setMeshVertices(
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const MeshDeformOptions& options)
{
    if (m_app)
        caustica::setMeshVertices(*m_app, entity, vertices, options);
}

void EngineApp::setMeshVerticesWorld(
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const MeshDeformOptions& options)
{
    if (m_app)
        caustica::setMeshVerticesWorld(*m_app, entity, vertices, options);
}

void EngineApp::requestMeshAccelRebuild(ecs::Entity entity, bool resetAccumulation)
{
    if (m_app)
        caustica::requestMeshAccelRebuild(*m_app, entity, resetAccumulation);
}

void EngineApp::requestFullAccelRebuild()
{
    if (m_app)
        caustica::requestFullAccelRebuild(*m_app);
}

uint32_t EngineApp::precacheRtFeaturePresets(bool showProgress)
{
    return m_app ? caustica::precacheRtFeaturePresets(*m_app, showProgress) : 0;
}

void EngineApp::setEnvMapOverrideSource(const std::string& path)
{
    if (m_app)
        caustica::setEnvMapOverrideSource(*m_app, path);
}

double EngineApp::sceneTime() const
{
    return m_viewState.sceneTime;
}

void EngineApp::setSceneTime(double seconds)
{
    m_viewState.sceneTime = seconds;
}

void EngineApp::setRealtimeMode(bool standaloneDenoiser, int realtimeAA)
{
    PathTracerSettings& s = m_renderAppState.settings;
    if (!s.RealtimeMode)
    {
        s.ResetAccumulation = true;
        s.ResetRealtimeCaches = true;
    }
    s.RealtimeMode = true;
    s.StandaloneDenoiser = standaloneDenoiser;
    s.RealtimeAA = realtimeAA;
}

void EngineApp::setReferenceMode(int spp, bool oidn, int oidnQuality, int oidnPasses, int oidnPrefilter)
{
    PathTracerSettings& s = m_renderAppState.settings;
    if (s.RealtimeMode)
        s.ResetAccumulation = true;
    s.RealtimeMode = false;
    if (spp > 0)
        s.AccumulationTarget = spp;
    s.ReferenceOIDNDenoiser = oidn;
    s.ReferenceOIDNQuality = oidnQuality;
    s.ReferenceOIDNPasses = oidnPasses;
    s.ReferenceOIDNPrefilter = oidnPrefilter;
    s.ReferenceOIDNDenoiserChanged = true;
}

bool EngineApp::prepareAnimationFrame(double sceneTime, bool importedAnimations, bool keyframes)
{
    if (!m_valid || !m_app || !std::isfinite(sceneTime))
        return false;

    PathTracerSettings& s = m_renderAppState.settings;
    const bool previousRealtime = s.RealtimeMode;
    const bool previousAnimations = s.EnableAnimations;
    const bool previousKeyframes = s.EnableKeyframes;

    setSceneTime(sceneTime);
    m_viewState.keyframeTime = sceneTime;
    s.RealtimeMode = true;
    s.EnableAnimations = importedAnimations;
    s.EnableKeyframes = keyframes;
    s.ResetRealtimeCaches = true;
    const bool ok = stepFrame(0.f);

    s.RealtimeMode = previousRealtime;
    s.EnableAnimations = previousAnimations;
    s.EnableKeyframes = previousKeyframes;
    s.ResetAccumulation = true;
    return ok;
}

bool EngineApp::accumulationCompleted() const
{
    return m_app && caustica::accumulationCompleted(*m_app);
}

int EngineApp::accumulationSampleIndex() const
{
    return m_app ? caustica::accumulationSampleIndex(*m_app) : 0;
}

math::uint2 EngineApp::renderSize() const
{
    return m_app ? caustica::renderSize(*m_app) : math::uint2{ 0, 0 };
}

float EngineApp::avgTimePerFrame() const
{
    return m_app ? caustica::avgTimePerFrame(*m_app) : 0.f;
}

std::string EngineApp::fpsInfo() const
{
    return m_app ? caustica::fpsInfo(*m_app) : std::string{};
}

std::string EngineApp::resolutionInfo() const
{
    return m_app ? caustica::resolutionInfo(*m_app) : std::string{};
}

caustica::rhi::Texture* EngineApp::ldrColorTexture() const
{
    return m_app ? caustica::ldrColorTexture(*m_app) : nullptr;
}

uint32_t EngineApp::frameIndex() const
{
    return m_device ? m_device->getFrameIndex() : 0;
}

namespace
{

bool ResolveLdrOutput(
    EngineApp& engine,
    caustica::rhi::Texture** outTexture,
    caustica::rhi::ResourceStates* outState)
{
    caustica::rhi::Texture* texture = engine.ldrColorTexture();
    caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::ShaderResource;
    if (!texture)
    {
        GpuDevice* device = engine.device();
        if (!device)
            return false;
        const uint32_t backBufferIndex = engine.surface()
            ? engine.surface()->getLastPresentedBackBufferIndex()
            : 0;
        texture = device->getBackBuffer(backBufferIndex);
        state = engine.window()
            ? caustica::rhi::ResourceStates::Present
            : caustica::rhi::ResourceStates::RenderTarget;
    }
    if (!texture)
        return false;
    *outTexture = texture;
    *outState = state;
    return true;
}

} // namespace

bool EngineApp::saveScreenshot(const std::filesystem::path& path)
{
    if (!m_app || !m_device)
        return false;

    caustica::rhi::Texture* texture = nullptr;
    caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::Unknown;
    if (!ResolveLdrOutput(*this, &texture, &state))
    {
        error("EngineApp: no current output texture");
        return false;
    }

    auto* infra = gpuSharedCaches(*m_app);
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        error("EngineApp: render device not initialized yet");
        return false;
    }

    if (!m_device->getDevice() || !m_device->getDevice()->waitForIdle())
    {
        error("EngineApp: GPU device lost or removed before screenshot");
        return false;
    }

    if (path.has_parent_path())
        ensureDirectoryExists(path.parent_path());

    return saveTextureToFile(
        m_device->getDevice(),
        *renderDevice,
        texture,
        state,
        path.string().c_str());
}

std::optional<LdrFramebuffer> EngineApp::readLdrFramebuffer()
{
    if (!m_app || !m_device)
        return std::nullopt;

    caustica::rhi::Device* device = m_device->getDevice();
    if (!device)
        return std::nullopt;

    caustica::rhi::Texture* texture = nullptr;
    caustica::rhi::ResourceStates textureState = caustica::rhi::ResourceStates::Unknown;
    if (!ResolveLdrOutput(*this, &texture, &textureState))
    {
        error("EngineApp: no current output texture for framebuffer readback");
        return std::nullopt;
    }

    auto* infra = gpuSharedCaches(*m_app);
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        error("EngineApp: render device not initialized yet");
        return std::nullopt;
    }

    if (!device->waitForIdle())
    {
        error("EngineApp: GPU device lost or removed before framebuffer readback");
        return std::nullopt;
    }

    caustica::rhi::TextureDesc desc = texture->getDesc();
    caustica::rhi::TextureHandle tempTexture;
    caustica::rhi::FramebufferHandle tempFramebuffer;

    caustica::rhi::CommandListHandle commandList = device->createCommandList();
    if (!commandList || !commandList->open())
        return std::nullopt;

    if (textureState != caustica::rhi::ResourceStates::Unknown)
        commandList->beginTrackingTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);

    switch (desc.format)
    {
    case caustica::rhi::Format::RGBA8_UNORM:
    case caustica::rhi::Format::SRGBA8_UNORM:
        tempTexture = texture;
        break;
    default:
        desc.format = caustica::rhi::Format::SRGBA8_UNORM;
        desc.isRenderTarget = true;
        desc.initialState = caustica::rhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        tempTexture = device->createTexture(desc);
        tempFramebuffer = device->createFramebuffer(caustica::rhi::FramebufferDesc().addColorAttachment(tempTexture));
        renderDevice->blit().blitTexture(commandList, tempFramebuffer, texture);
        break;
    }

    caustica::rhi::TextureDesc stagingDesc = desc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.isTypeless = false;
    stagingDesc.initialState = caustica::rhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "LdrFramebuffer Staging";

    caustica::rhi::StagingTextureHandle stagingTexture =
        device->createStagingTexture(stagingDesc, caustica::rhi::CpuAccessMode::Read);
    if (!stagingTexture)
    {
        commandList->close();
        return std::nullopt;
    }

    commandList->copyTexture(stagingTexture, caustica::rhi::TextureSlice(), tempTexture, caustica::rhi::TextureSlice());

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->setTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
        commandList->commitBarriers();
    }

    commandList->close();
    device->executeCommandList(commandList);

    if (!device->waitForIdle())
        return std::nullopt;

    size_t rowPitch = 0;
    const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(
        stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
    if (!mapped)
        return std::nullopt;

    LdrFramebuffer result;
    result.width = desc.width;
    result.height = desc.height;
    result.channels = 4;
    result.pixels.resize(size_t(desc.width) * size_t(desc.height) * 4u);

    const size_t dstStride = size_t(desc.width) * 4u;
    for (uint32_t row = 0; row < desc.height; ++row)
    {
        std::memcpy(
            result.pixels.data() + size_t(row) * dstStride,
            mapped + size_t(row) * rowPitch,
            dstStride);
    }

    device->unmapStagingTexture(stagingTexture);
    return result;
}

} // namespace caustica
