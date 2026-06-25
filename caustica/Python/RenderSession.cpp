#include "RenderSession.h"

#if CAUSTICA_WITH_PYTHON

#include "AdvancedPathTracer.h"
#include "EditorApplication.h"
#include "PathTracerApp.h"
#include <SampleCommon/LocalConfig.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>
#include <assets/loader/ShaderPackFileSystem.h>

#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/CommonRenderPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <platform/glfw_window.h>
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif

#include <GLFW/glfw3.h>
#include <json/json.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#if CAUSTICA_WITH_DX12
#include <d3d12.h>
#include <wrl/client.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif



namespace
{
    constexpr double c_HeadlessFrameTimeSeconds = 1.0 / 60.0;

    class PathTracerFrameDriver : public caustica::Application
    {
    public:
        PathTracerFrameDriver(caustica::GpuDevice* dm, caustica::Window* window, PathTracerApp* scene)
            : caustica::Application(dm, window)
            , m_scene(scene)
        { }

        void syncToBackBuffer()
        {
            caustica::GpuDevice* dm = getGpuDevice();
            if (!dm || !m_scene)
                return;

            const auto& params = dm->GetDeviceParams();
            notifyBackBufferResizing();
            notifyBackBufferResized(params.backBufferWidth, params.backBufferHeight, params.swapChainSampleCount);
        }

    protected:
        void onUpdate(float elapsedTimeSeconds, bool windowFocused) override
        {
            if (m_scene && (windowFocused || m_scene->ShouldAnimateUnfocused()))
            {
                m_scene->Animate(elapsedTimeSeconds);
                m_scene->SetLatewarpOptions();
            }
        }

        void onRender() override
        {
            caustica::GpuDevice* dm = getGpuDevice();
            if (!m_scene || !dm)
                return;

            m_scene->Render(dm->GetCurrentFramebuffer(m_scene->SupportsDepthBuffer()));
        }

        bool shouldRenderWhenUnfocused() const override
        {
            return m_scene && m_scene->ShouldRenderUnfocused();
        }

        void onBackBufferResizing() override
        {
            if (m_scene)
                m_scene->BackBufferResizing();
        }

        void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override
        {
            if (m_scene)
                m_scene->BackBufferResized(width, height, sampleCount);
        }

        void onDisplayScaleChanged(float scaleX, float scaleY) override
        {
            if (m_scene)
                m_scene->DisplayScaleChanged(scaleX, scaleY);
        }

    private:
        PathTracerApp* m_scene = nullptr;
    };

    void AppendUnique(std::vector<std::string>& values, const std::string& value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }

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
            DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (length > 0 && length < path.size())
                return std::filesystem::path(path.data()).parent_path();
        }
#else
        Dl_info info = {};
        if (dladdr(reinterpret_cast<void*>(&GetCurrentModuleDirectory), &info) && info.dli_fname)
            return std::filesystem::path(info.dli_fname).parent_path();
#endif

        return caustica::GetDirectoryWithExecutable();
    }

    std::filesystem::path ResolveRuntimeDirectory()
    {
        std::filesystem::path moduleDirectory = GetCurrentModuleDirectory();
        if (std::filesystem::exists(moduleDirectory / "ShaderPrecompiled"))
            return moduleDirectory;

        std::filesystem::path executableDirectory = caustica::GetDirectoryWithExecutable();
        if (std::filesystem::exists(executableDirectory / "ShaderPrecompiled"))
            return executableDirectory;

        return moduleDirectory;
    }

    std::filesystem::path ResolveResourceRoot(const std::filesystem::path& runtimeDirectory)
    {
        if (std::filesystem::exists(runtimeDirectory / c_AssetsFolder))
            return runtimeDirectory;

        std::filesystem::path parentDirectory = runtimeDirectory.parent_path();
        if (std::filesystem::exists(parentDirectory / c_AssetsFolder))
            return parentDirectory;

        return caustica::GetDirectoryWithExecutable();
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

    std::string BuildBuiltinDefaultSceneJson(const std::string& builtinModel)
    {
        Json::Value root(Json::objectValue);
        root["models"].append(std::string("builtin:") + NormalizeBuiltinModelName(builtinModel));

        Json::Value modelNode(Json::objectValue);
        modelNode["name"] = "DefaultBuiltinModel";
        modelNode["model"] = 0;
        root["graph"].append(modelNode);

        Json::Value sun(Json::objectValue);
        sun["name"] = "Sun";
        sun["type"] = "DirectionalLight";
        sun["rotation"] = MakeFloatArray({ -0.23053891f, -0.15879166f, -0.6890466f, 0.6684697f });
        sun["angularSize"] = 1.5f;
        sun["color"] = MakeFloatArray({ 1.0f, 0.96f, 0.9f });
        sun["irradiance"] = 4.0f;

        Json::Value fill(Json::objectValue);
        fill["name"] = "Fill";
        fill["type"] = "PointLight";
        fill["translation"] = MakeFloatArray({ 0.0f, 2.5f, 3.0f });
        fill["color"] = MakeFloatArray({ 1.0f, 0.95f, 0.85f });
        fill["intensity"] = 30.0f;
        fill["radius"] = 0.05f;
        fill["range"] = 10.0f;

        Json::Value lights(Json::objectValue);
        lights["name"] = "Lights";
        lights["children"].append(sun);
        lights["children"].append(fill);
        root["graph"].append(lights);

        Json::Value camera(Json::objectValue);
        camera["name"] = "Default";
        camera["type"] = "PerspectiveCameraEx";
        camera["translation"] = MakeFloatArray({ 0.0f, 1.15f, 5.0f });
        camera["rotation"] = MakeFloatArray({ 0.0f, 0.0f, 0.0f, 1.0f });
        camera["verticalFov"] = 0.7f;
        camera["zNear"] = 0.001f;
        camera["exposureCompensation"] = 1.0f;
        camera["enableAutoExposure"] = false;

        Json::Value cameras(Json::objectValue);
        cameras["name"] = "Cameras";
        cameras["children"].append(camera);
        root["graph"].append(cameras);

        Json::Value settings(Json::objectValue);
        settings["name"] = "SampleSettings";
        settings["type"] = "SampleSettings";
        settings["realtimeMode"] = true;
        settings["startingCamera"] = -1;
        root["graph"].append(settings);

        return caustica::json::ToString(root);
    }

    std::string PrepareSceneArgument(const std::string& sceneArgument)
    {
        const std::string trimmed = TrimCopy(sceneArgument);
        if (trimmed.empty())
            return sceneArgument;

        if (IsBuiltinModelReference(trimmed))
            return BuildBuiltinDefaultSceneJson(trimmed);

        return sceneArgument;
    }

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    std::string GetAgilitySDKPath()
    {
        std::string sdkPath = (ResolveRuntimeDirectory() / "D3D12").string();
        if (!sdkPath.empty() && sdkPath.back() != '\\' && sdkPath.back() != '/')
            sdkPath += "\\";
        return sdkPath;
    }

    bool EnableD3D12ExperimentalShaderModels(ID3D12DeviceFactory* factory)
    {
        static const UUID D3D12ExperimentalShaderModels = { 0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f} };
        UUID features[] = { D3D12ExperimentalShaderModels };

        HRESULT hr = factory
            ? factory->EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr)
            : D3D12EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr);
        if (FAILED(hr))
        {
            if (factory && hr == E_NOINTERFACE)
                return false;
            caustica::warning("RenderSession: D3D12 experimental shader models could not be enabled, HRESULT = 0x%08x", unsigned(hr));
            return false;
        }
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> CreateD3D12AgilityDeviceFactory()
    {
        const std::string sdkPath = GetAgilitySDKPath();

        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> sdkConfig1;
        HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig1));
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<ID3D12DeviceFactory> factory;
            hr = sdkConfig1->CreateDeviceFactory(
                CAUSTICA_D3D_AGILITY_SDK_VERSION,
                sdkPath.c_str(),
                IID_PPV_ARGS(&factory));

            if (SUCCEEDED(hr) && factory)
            {
                EnableD3D12ExperimentalShaderModels(factory.Get());
                return factory;
            }

            caustica::warning("RenderSession: ID3D12SDKConfiguration1::CreateDeviceFactory('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
        }
        else
        {
            caustica::warning("RenderSession: D3D12GetInterface(ID3D12SDKConfiguration1) failed, HRESULT = 0x%08x", unsigned(hr));
        }

        // Fallback for older runtimes. This works when the host process has
        // not already locked D3D12 to the system SDK.
        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
        hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));
        if (FAILED(hr))
        {
            caustica::warning("RenderSession: D3D12GetInterface(ID3D12SDKConfiguration) failed, HRESULT = 0x%08x", unsigned(hr));
            return nullptr;
        }

        hr = sdkConfig->SetSDKVersion(CAUSTICA_D3D_AGILITY_SDK_VERSION, sdkPath.c_str());
        if (FAILED(hr))
        {
            caustica::warning("RenderSession: ID3D12SDKConfiguration::SetSDKVersion('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
            return nullptr;
        }

        EnableD3D12ExperimentalShaderModels(nullptr);
        return nullptr;
    }
#endif

    caustica::DeviceCreationParameters MakeDeviceParams(const RenderSession::Config& cfg)
    {
        caustica::DeviceCreationParameters p;
        p.backBufferWidth        = cfg.width;
        p.backBufferHeight       = cfg.height;
        p.swapChainSampleCount   = 1;
        p.swapChainBufferCount   = c_SwapchainCount;
        p.startFullscreen        = false;
        p.startBorderless        = false;
        p.vsyncEnabled           = false;       // headless => no need for vsync
        p.enableRayTracingExtensions = true;
        p.adapterIndex           = cfg.adapterIndex;
        p.headlessDevice         = cfg.headless;

        if (cfg.debug)
        {
            p.enableDebugRuntime         = true;
            p.enableNvrhiValidationLayer = true;
        }

        p.supportExplicitDisplayScaling = true;

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        p.featureLevel = D3D_FEATURE_LEVEL_12_2;
#elif CAUSTICA_WITH_DX12
        p.featureLevel = D3D_FEATURE_LEVEL_12_1;
#endif

#if CAUSTICA_WITH_VULKAN
#if CAUSTICA_WITH_NATIVE_DLSS
        caustica::render::DLSS::GetRequiredVulkanExtensions(
            p.requiredVulkanInstanceExtensions,
            p.requiredVulkanDeviceExtensions);
#endif
        AppendUnique(p.requiredVulkanDeviceExtensions, "VK_KHR_buffer_device_address");
        AppendUnique(p.requiredVulkanDeviceExtensions, "VK_KHR_format_feature_flags2");
        p.ignoredVulkanValidationMessageLocations.push_back(0x0000000023e43bb7);
        p.ignoredVulkanValidationMessageLocations.push_back(0x000000000609a13b);
        p.ignoredVulkanValidationMessageLocations.push_back(0x00000000c5a3822a);
        p.ignoredVulkanValidationMessageLocations.push_back(0x00000000591f70f2);
        p.ignoredVulkanValidationMessageLocations.push_back(0x000000005e6e827d);
#endif

#if CAUSTICA_WITH_STREAMLINE
        p.checkStreamlineSignature = true;
        p.streamlineAppId = 231313132;
#endif

        return p;
    }

    nvrhi::GraphicsAPI ResolveGraphicsAPI(const RenderSession::Config& cfg)
    {
#if CAUSTICA_WITH_DX12 && CAUSTICA_WITH_VULKAN
        return cfg.useVulkan ? nvrhi::GraphicsAPI::VULKAN : nvrhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
        if (!cfg.useVulkan)
            caustica::warning("RenderSession: DX12 was requested but this build only has Vulkan; using Vulkan.");
        return nvrhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX12
        if (cfg.useVulkan)
            caustica::warning("RenderSession: Vulkan was requested but this build only has DX12; using DX12.");
        return nvrhi::GraphicsAPI::D3D12;
#else
        static_assert(CAUSTICA_WITH_DX12 || CAUSTICA_WITH_VULKAN, "RTXPT requires at least one graphics backend");
#endif
    }
}

namespace caustica_py
{
    std::string BuiltinSceneJson(const std::string& builtinModel)
    {
        return BuildBuiltinDefaultSceneJson(builtinModel);
    }
}

RenderSession::RenderSession(const Config& cfg)
    : m_config(cfg)
{
    m_config.scene = PrepareSceneArgument(cfg.scene);

    // Mirror command-line semantics: this is the configuration the rest of
    // the renderer (CaptureScriptManager, Sample::Init, ...) consumes.
    m_cmdLine.width             = uint32_t(cfg.width);
    m_cmdLine.height            = uint32_t(cfg.height);
    m_cmdLine.noWindow          = cfg.headless;
    m_cmdLine.useVulkan         = cfg.useVulkan;
    m_cmdLine.adapterIndex      = cfg.adapterIndex;
    m_cmdLine.debug             = cfg.debug;
    m_cmdLine.nonInteractive    = cfg.nonInteractive;
    m_cmdLine.scene             = m_config.scene;
    m_cmdLine.OverrideToReferenceMode = !cfg.realtimeMode;
    m_cmdLine.OverrideToRealtimeMode  =  cfg.realtimeMode;
    m_cmdLine.ReferenceSamplesPerPixel = cfg.accumulationTarget;

    if (cfg.nonInteractive)
    {
        caustica::EnableOutputToMessageBox(false);
        caustica::EnableOutputToConsole(true);
        caustica::SetMinSeverity(caustica::Severity::Warning);
        HelpersSetNonInteractive();
    }

    if (!InitDevice())
    {
        caustica::error("RenderSession: failed to initialize the graphics device");
        return;
    }

    if (!InitRenderer())
    {
        caustica::error("RenderSession: failed to initialize the renderer");
        return;
    }

    m_initialized = true;

    // If a scene was specified up-front, InitRenderer already requested it.
    // Wait for the first rendered frame instead of reloading the same scene.
    if (!m_config.scene.empty())
        WaitUntilReady();
}

RenderSession::~RenderSession()
{
    Shutdown();
}

bool RenderSession::InitDevice()
{
    nvrhi::GraphicsAPI api = ResolveGraphicsAPI(m_config);

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (api == nvrhi::GraphicsAPI::D3D12)
        m_d3d12DeviceFactory = CreateD3D12AgilityDeviceFactory();
#endif

    m_deviceManager.reset(caustica::GpuDevice::Create(api));
    if (!m_deviceManager)
    {
        caustica::error("RenderSession: GpuDevice::Create returned null");
        return false;
    }
    m_deviceManager->SetFrameTimeUpdateInterval(1.0f);

    auto deviceParams = MakeDeviceParams(m_config);
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (api == nvrhi::GraphicsAPI::D3D12 && m_d3d12DeviceFactory)
        deviceParams.d3d12DeviceFactory = m_d3d12DeviceFactory.Get();
#endif

    if (m_config.headless)
    {
        if (!m_deviceManager->CreateHeadlessDevice(deviceParams))
        {
            caustica::error("RenderSession: failed to create headless device and offscreen back buffers");
            return false;
        }
    }
    else
    {
        caustica::GlfwWindow::makeDefault();

        caustica::WindowDesc windowDesc;
        windowDesc.Width = deviceParams.backBufferWidth;
        windowDesc.Height = deviceParams.backBufferHeight;
        windowDesc.Title = "caustica_py";
        windowDesc.RenderAPI = static_cast<int>(api);
        windowDesc.VSync = deviceParams.vsyncEnabled;

        m_Window.reset(caustica::Window::create(windowDesc));
        if (!m_Window || !m_Window->hasInitialised())
        {
            caustica::error("RenderSession: failed to create platform window");
            return false;
        }

        if (!m_deviceManager->CreateDeviceAndSwapChain(deviceParams, m_Window.get()))
        {
            caustica::error("RenderSession: failed to create device and swap chain");
            return false;
        }
    }

    auto device = m_deviceManager->GetDevice();
    if (!device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
    {
        caustica::error("RenderSession: the graphics device does not support Ray Tracing Pipelines");
        return false;
    }

    if (!device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        caustica::error("RenderSession: the graphics device does not support Ray Queries");
        return false;
    }

    return true;
}

bool RenderSession::InitRenderer()
{
    // Shader factory pulls precompiled shaders from the host executable's
    // ShaderPrecompiled folder - this folder is assumed to live next to the
    // .pyd / caustica.exe binary.
    const char* shaderTypeName = caustica::GetShaderTypeName(m_deviceManager->GetGraphicsAPI());
    const std::filesystem::path appDirectory = ResolveRuntimeDirectory();
    SetRuntimeDirectoryOverride(appDirectory);
    SetLocalPathBaseOverride(ResolveResourceRoot(appDirectory));
    std::filesystem::path engineShaderPath    = appDirectory / "ShaderPrecompiled/engine"    / shaderTypeName;
    std::filesystem::path appShaderPath       = appDirectory / "ShaderPrecompiled/caustica"  / shaderTypeName;
    std::filesystem::path nrdShaderPath       = appDirectory / "ShaderPrecompiled/nrd"       / shaderTypeName;
    std::filesystem::path ommShaderPath       = appDirectory / "ShaderPrecompiled/omm"       / shaderTypeName;

    auto rootFS = std::make_shared<caustica::RootFileSystem>();
    const std::filesystem::path shaderPackPath = appDirectory / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, "ShaderPrecompiled");
    const bool shaderPackHasCurrentLayout = shaderPackFS->isOpen() && shaderPackFS->fileExists("caustica/caustica/shaders/render/Misc/DebugLines_main_vs.bin");
    if (shaderPackFS->isOpen() && !shaderPackHasCurrentLayout)
    {
        caustica::warning("Shader pack '%s' does not match the current shader layout; falling back to ShaderPrecompiled directories",
            shaderPackPath.string().c_str());
    }

    if (shaderPackHasCurrentLayout)
    {
        rootFS->mount("/ShaderPrecompiled", shaderPackFS);
    }
    else
    {
        rootFS->mount("/ShaderPrecompiled/engine", engineShaderPath);
        rootFS->mount("/ShaderPrecompiled/caustica", appShaderPath);
        rootFS->mount("/ShaderPrecompiled/nrd",   nrdShaderPath);
        rootFS->mount("/ShaderPrecompiled/omm",   ommShaderPath);
    }

    auto device = m_deviceManager->GetDevice();
    m_shaderFactory = std::make_shared<caustica::ShaderFactory>(device, rootFS, "/ShaderPrecompiled");

    m_renderer = std::make_unique<AdvancedPathTracer>(*m_deviceManager, m_cmdLine, m_sampleUIData);
    InitializeSampleUIDataFromCommandLine(m_sampleUIData, m_cmdLine);
    LocalConfig::PostAppInit(m_sampleUIData);

    // Pick whichever scene the user requested (or fall back to the default)
    // default).  This loads the actual scene file asynchronously inside
    // Sample::Init().
    std::string preferredScene = m_config.scene.empty()
        ? std::string("default.json")
        : m_config.scene;

    m_renderer->Init(preferredScene, m_shaderFactory);

    auto frameDriver = std::make_unique<PathTracerFrameDriver>(
        m_deviceManager.get(),
        m_config.headless ? nullptr : m_Window.get(),
        m_renderer.get());
    frameDriver->beforePresent =
        [this](caustica::GpuDevice& manager, uint32_t) {
            m_lastRenderedBackBufferIndex = manager.GetCurrentBackBufferIndex();
        };
    frameDriver->syncToBackBuffer();
    m_AppLoop = std::move(frameDriver);

    return true;
}

void RenderSession::Shutdown()
{
    if (m_deviceManager)
        m_deviceManager->setFrameDriver(nullptr);

    m_renderer.reset();
    m_shaderFactory.reset();
    m_AppLoop.reset();

    if (m_deviceManager)
    {
        m_deviceManager->ReleaseWindowOwnership();
        m_deviceManager->Shutdown();
        m_deviceManager.reset();
    }

    m_Window.reset();
    m_initialized = false;
}

bool RenderSession::LoadScene(const std::string& sceneName, bool waitUntilReady)
{
    if (!m_initialized || !m_renderer)
        return false;

    m_renderer->SetCurrentScene(PrepareSceneArgument(sceneName), /*forceReload=*/true);

    if (waitUntilReady)
        return WaitUntilReady();
    return true;
}

bool RenderSession::WaitUntilReady(int maxFrames)
{
    if (!m_initialized || !m_deviceManager)
        return false;

    // Loading + first-frame setup may take quite a few frames - keep
    // pumping until the renderer reports its accumulation index moved
    // past 0 (= scene fully loaded and at least one image was produced).
    for (int i = 0; i < maxFrames; ++i)
    {
        Step(0.0f);
        if (m_renderer && m_renderer->IsSceneLoaded() && !m_renderer->IsSceneLoading())
            return true;
    }
    caustica::warning("RenderSession: scene did not finish loading within %d frames", maxFrames);
    return false;
}

bool RenderSession::Step(float dt)
{
    if (!m_initialized || !m_deviceManager)
        return false;

    if (m_Window && m_Window->getExit())
        return false;

    const bool frameOk = dt >= 0.0f
        ? m_AppLoop->stepFrame(double(dt))
        : m_AppLoop->stepFrame(m_config.headless ? c_HeadlessFrameTimeSeconds : -1.0);

    if (!frameOk)
        return false;

    // Headless Python stepping can outrun the GPU and cause resource hazards
    // (e.g. screenshot readback or auto-exposure buffer maps). Serialize frames.
    if (m_config.headless)
    {
        if (!m_deviceManager->GetDevice()->waitForIdle())
        {
            caustica::error("RenderSession: GPU device lost or removed");
            return false;
        }
    }

    window = m_deviceManager->GetWindow();
    return !window || !glfwWindowShouldClose(window);
}

bool RenderSession::StepN(int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        if (!Step())
            return false;
    }
    return true;
}

int RenderSession::StepUntilAccumulated(int maxFrames)
{
    if (!m_initialized || !m_renderer)
        return 0;

    // Force reference / accumulation mode so we know "done" actually means
    // the SPP target has been reached.
    m_sampleUIData.ResetAccumulation = true;

    int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, m_sampleUIData.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        if (!Step()) break;
        ++frames;
        if (m_renderer->AccumulationCompleted())
            break;
    }
    return frames;
}

bool RenderSession::SaveScreenshot(const std::string& outputPath)
{
    if (!m_initialized || !m_deviceManager || !m_renderer)
        return false;

    // Prefer the renderer-owned final LDR target. It is the source of the
    // frame blit, so it does not depend on headless backbuffer rotation.
    nvrhi::ITexture* tex = m_renderer->GetLdrColorTexture();
    nvrhi::ResourceStates state = nvrhi::ResourceStates::ShaderResource;

    if (!tex)
    {
        uint32_t backBufferIndex = m_lastRenderedBackBufferIndex;
        if (backBufferIndex == UINT32_MAX)
            backBufferIndex = m_deviceManager->GetCurrentBackBufferIndex();

        tex = m_deviceManager->GetBackBuffer(backBufferIndex);
        state = m_config.headless
            ? nvrhi::ResourceStates::RenderTarget
            : nvrhi::ResourceStates::Present;
    }

    if (!tex)
    {
        caustica::error("RenderSession: no current output texture");
        return false;
    }

    auto commonPasses = m_renderer->GetCommonPasses();
    if (!commonPasses)
    {
        caustica::error("RenderSession: common passes not initialized yet");
        return false;
    }

    // SaveTextureToFile creates its own command list. Wait for the last rendered
    // frame to finish so LdrColor is not still in use by an in-flight submit.
    if (!m_deviceManager->GetDevice()->waitForIdle())
    {
        caustica::error("RenderSession: GPU device lost or removed before screenshot");
        return false;
    }

    std::filesystem::path p(outputPath);
    if (p.has_parent_path())
        EnsureDirectoryExists(p.parent_path());

    return caustica::SaveTextureToFile(
        m_deviceManager->GetDevice(),
        commonPasses.get(),
        tex,
        state,
        outputPath.c_str());
}

bool RenderSession::SetCamera(const caustica::math::float3& pos,
                              const caustica::math::float3& dir,
                              const caustica::math::float3& up)
{
    if (!m_renderer) return false;

    auto v3 = [](const caustica::math::float3& v) {
        return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
    };
    std::string s = v3(pos) + "," + v3(dir) + "," + v3(up);
    return m_renderer->SetCurrentCameraPosDirUp(s);
}

void RenderSession::SetCameraFOV(float verticalFovDegrees)
{
    if (m_renderer)
        m_renderer->SetCameraVerticalFOV(caustica::math::radians(verticalFovDegrees));
}

void RenderSession::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (m_renderer)
        m_renderer->SetCameraIntrinsics(fx, fy, cx, cy, width, height);
}

#endif // CAUSTICA_WITH_PYTHON
