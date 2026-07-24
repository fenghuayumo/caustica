#include <engine/EngineApp.h>

#include <engine/DefaultPlugins.h>
#include <engine/EntryPoint.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <platform/window.h>

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
    if (std::filesystem::exists(moduleDirectory / "ShaderPrecompiled"))
        return moduleDirectory;

    const std::filesystem::path executableDirectory = getDirectoryWithExecutable();
    if (std::filesystem::exists(executableDirectory / "ShaderPrecompiled"))
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

    m_viewStatePtr = m_desc.viewState ? m_desc.viewState : &m_viewState;
    m_diagnosticsPtr = m_desc.diagnostics ? m_desc.diagnostics : &m_diagnostics;
    m_renderStatePtr = m_desc.renderState ? m_desc.renderState : &m_renderAppState;
    m_cmdLinePtr = m_desc.cmdLine ? m_desc.cmdLine : &m_cmdLine;

    const std::filesystem::path runtimeDirectory = m_desc.runtimeDirectory.empty()
        ? ResolveDefaultRuntimeDirectory()
        : m_desc.runtimeDirectory;
    const std::filesystem::path resourceRoot = m_desc.resourceRoot.empty()
        ? ResolveDefaultResourceRoot(runtimeDirectory)
        : m_desc.resourceRoot;

    setRuntimeDirectoryOverride(runtimeDirectory);
    setLocalPathBaseOverride(resourceRoot);

    if (!m_desc.cmdLine)
    {
        m_cmdLine.width = m_desc.width;
        m_cmdLine.height = m_desc.height;
        m_cmdLine.noWindow = m_desc.headless;
        m_cmdLine.useVulkan = m_desc.useVulkan;
        m_cmdLine.adapterIndex = m_desc.adapterIndex;
        m_cmdLine.debug = m_desc.debugDevice;
        m_cmdLine.scene = m_desc.scene;
        m_cmdLine.syncRender = !m_desc.dedicatedRenderThread;
    }

    if (m_desc.device)
    {
        m_device = m_desc.device;
        m_window = m_desc.window;
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
        createDesc.adapterIndex = m_desc.adapterIndex;
        createDesc.enableDebug = m_desc.debugDevice;
        createDesc.startFullscreen = m_desc.fullscreen;
        if (m_desc.headless)
            createDesc.vsyncEnabled = false;
#if CAUSTICA_WITH_DX12
        createDesc.d3d12DeviceFactory = m_desc.d3d12DeviceFactory;
#endif

        GpuDeviceCreateResult graphicsResult = GpuDevice::createInitialized(createDesc);
        if (!graphicsResult.gpuDevice)
        {
            error("EngineApp: failed to create GPU device");
            return false;
        }

        m_ownedDevice = std::move(graphicsResult.gpuDevice);
        m_ownedWindow = std::move(graphicsResult.window);
        m_device = m_ownedDevice.get();
        m_window = m_ownedWindow.get();
        m_ownsDevice = true;
    }

    m_viewStatePtr->progressLoading.start("Initializing...");
    m_viewStatePtr->progressLoading.Set(50);

    const std::string preferredScene = m_desc.scene.empty() ? std::string("default.json") : m_desc.scene;

    m_app = std::make_unique<App>(m_device, m_desc.headless ? nullptr : m_window);
    m_app->addPlugins(DefaultPlugins{SceneAppConfig{
        .viewState = *m_viewStatePtr,
        .diagnostics = *m_diagnosticsPtr,
        .preferredScene = preferredScene,
        .renderState = m_renderStatePtr,
        .cmdLine = m_cmdLinePtr,
        .applyCmdLineToRenderState = m_desc.applyCmdLineToRenderState,
        .hasSceneCallbacks = m_desc.hasSceneCallbacks,
        .sceneCallbacks = m_desc.sceneCallbacks,
    }});
    m_app->setUseDedicatedRenderThread(m_desc.dedicatedRenderThread && !m_desc.headless);

    if (m_desc.finishStartup)
    {
        if (!m_app->finishStartup())
        {
            error("EngineApp: finishStartup failed");
            shutdown();
            return false;
        }
    }

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
    m_app->run();
    shutdown();
}

bool EngineApp::stepFrame(float dtSeconds)
{
    if (!m_valid || !m_app)
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

    if (m_device)
        m_device->setFrameDriver(nullptr);

    m_app.reset();

    if (m_ownsDevice && m_ownedDevice)
    {
        m_ownedDevice->releaseWindowOwnership();
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

void EngineApp::setScene(const std::string& name, bool forceReload)
{
    if (m_app)
        caustica::setCurrentScene(*m_app, name, forceReload);
}

std::shared_ptr<Scene> EngineApp::scene() const
{
    return m_app ? caustica::activeScene(*m_app) : nullptr;
}

bool EngineApp::isSceneLoaded() const
{
    return m_app && caustica::isSceneLoaded(*m_app);
}

bool EngineApp::isSceneLoading() const
{
    return m_app && caustica::isSceneLoading(*m_app);
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
    return *m_renderStatePtr;
}

const render::RenderAppState& EngineApp::renderAppState() const
{
    return *m_renderStatePtr;
}

CommandLineOptions& EngineApp::commandLine()
{
    return *m_cmdLinePtr;
}

const CommandLineOptions& EngineApp::commandLine() const
{
    return *m_cmdLinePtr;
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

caustica::rhi::ITexture* EngineApp::ldrColorTexture() const
{
    return m_app ? caustica::ldrColorTexture(*m_app) : nullptr;
}

uint32_t EngineApp::frameIndex() const
{
    return m_device ? m_device->getFrameIndex() : 0;
}

} // namespace caustica
