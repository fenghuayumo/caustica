#include <engine/EngineApp.h>
#include <backend/GpuSurface.h>
#include <engine/DefaultPlugins.h>
#include <engine/EntryPoint.h>
#include <engine/SceneQuery.h>
#include <engine/SceneStartup.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <platform/window.h>
#include <rhi/rhi.h>

#include <array>
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

} // namespace

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

    const std::string preferredScene = m_desc.scene.empty() ? std::string("default.scene.json") : m_desc.scene;

    m_app = std::make_unique<App>(m_device, m_desc.headless ? nullptr : m_window, m_surface);
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
        caustica::setCurrentScene(*m_app, name, forceReload);
}

bool EngineApp::isSceneLoaded() const
{
    return m_app && caustica::isSceneLoaded(*m_app);
}

bool EngineApp::isSceneLoading() const
{
    return m_app && caustica::isSceneLoading(*m_app);
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

bool EngineApp::setCameraPosDirUp(const std::string& value)
{
    return m_app && caustica::setCurrentCameraPosDirUp(*m_app, value);
}

void EngineApp::setCameraVerticalFOV(float radians)
{
    if (m_app)
        caustica::setCameraVerticalFOV(*m_app, radians);
}

void EngineApp::setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (m_app)
        caustica::setCameraIntrinsics(*m_app, fx, fy, cx, cy, width, height);
}

bool EngineApp::accumulationCompleted() const
{
    return m_app && caustica::accumulationCompleted(*m_app);
}

caustica::rhi::Texture* EngineApp::ldrColorTexture() const
{
    return m_app ? caustica::ldrColorTexture(*m_app) : nullptr;
}

uint32_t EngineApp::frameIndex() const
{
    return m_device ? m_device->getFrameIndex() : 0;
}

} // namespace caustica
