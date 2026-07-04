#include "RenderSession.h"

#if CAUSTICA_WITH_PYTHON

#include "SceneEditorFrameExtension.h"
#include "SceneEditor.h"
#include "EditorRuntime.h"
#include <engine/EngineFrameApplication.h>
#include <engine/Application.h>
#include <backend/GpuDevice.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <common/LocalConfig.h>
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
#include <assets/loader/TextureLoader.h>
#include <render/Core/BindingCache.h>
#include <core/vfs/VFS.h>
#include <engine/UserInterfaceUtils.h>
#include <platform/glfw_window.h>
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif

#include <GLFW/glfw3.h>
#include <json/json.h>
#include <core/json.h>
#include <platform/glfw_window.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using caustica::editor::SceneEditor;
using caustica::editor::registerEditorRuntime;
using caustica::editor::EditorSceneSubsystemConfig;

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

    caustica::GpuDeviceCreateDesc createDesc{};
    createDesc.api = api;
    createDesc.headless = m_config.headless;
    createDesc.windowTitle = "caustica_py";
    createDesc.backBufferWidth = m_config.width;
    createDesc.backBufferHeight = m_config.height;
    createDesc.adapterIndex = m_config.adapterIndex;
    createDesc.enableDebug = m_config.debug;
    if (m_config.headless)
        createDesc.vsyncEnabled = false;

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (api == nvrhi::GraphicsAPI::D3D12 && m_d3d12DeviceFactory)
        createDesc.d3d12DeviceFactory = m_d3d12DeviceFactory.Get();
#endif

    caustica::GpuDeviceCreateResult graphicsResult = caustica::GpuDevice::CreateInitialized(createDesc);
    if (!graphicsResult.gpuDevice)
        return false;

    m_deviceManager = std::move(graphicsResult.gpuDevice);
    m_Window = std::move(graphicsResult.window);
    return true;
}

bool RenderSession::InitRenderer()
{
    const std::filesystem::path appDirectory = ResolveRuntimeDirectory();
    SetRuntimeDirectoryOverride(appDirectory);
    SetLocalPathBaseOverride(ResolveResourceRoot(appDirectory));

    m_renderer = std::make_unique<caustica::editor::SceneEditor>(m_cmdLine, m_sessionState, m_editorUIState, m_sessionDiagnostics);

    m_sceneEditorFrameExtension = std::make_unique<caustica::editor::SceneEditorFrameExtension>(*m_renderer);
    m_framePasses = { m_sceneEditorFrameExtension.get() };

    std::string preferredScene = m_config.scene.empty()
        ? std::string("default.json")
        : m_config.scene;

    m_engine = std::make_unique<caustica::Engine>();
    registerEditorRuntime(*m_engine, EditorSceneSubsystemConfig{
        .sceneEditor = *m_renderer,
        .diagnostics = m_sessionDiagnostics,
        .framePasses = m_framePasses,
        .preferredScene = preferredScene,
        .sessionState = &m_sessionState,
        .cmdLine = &m_cmdLine,
        .applyCmdLineToSessionState = true,
        .postAppInit = false,
    });

    m_AppLoop = std::make_unique<caustica::EngineFrameApplication>(
        m_engine.get(),
        m_deviceManager.get(),
        m_config.headless ? nullptr : m_Window.get());
    m_AppLoop->setUseDedicatedRenderThread(!m_cmdLine.syncRender && !m_config.headless);
    m_AppLoop->beforePresent =
        [this](caustica::GpuDevice& manager, uint32_t) {
            m_lastRenderedBackBufferIndex = manager.GetCurrentBackBufferIndex();
        };

    if (!m_engine->initialize(caustica::EngineInitContext{
            .gpuDevice = m_deviceManager.get(),
            .window = m_config.headless ? nullptr : m_Window.get(),
            .application = m_AppLoop.get(),
        }))
    {
        return false;
    }

    m_AppLoop->syncSwapChain();

    return true;
}

void RenderSession::Shutdown()
{
    if (m_deviceManager)
        m_deviceManager->setFrameDriver(nullptr);

    m_AppLoop.reset();
    m_renderer.reset();
    m_sceneEditorFrameExtension.reset();
    m_framePasses.clear();
    if (m_engine)
        m_engine->shutdown();
    m_engine.reset();

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

    GLFWwindow* window = m_deviceManager->GetWindow();
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
    m_sessionState.ResetAccumulation = true;

    int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, m_sessionState.AccumulationTarget + 128);

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
