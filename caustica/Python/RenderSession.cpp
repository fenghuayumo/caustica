#include "RenderSession.h"

#if CAUSTICA_WITH_PYTHON

#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <engine/RenderFrameApi.h>
#include <engine/internal/SceneApiInternal.h>
#include <scene/SceneManager.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/RenderDevice.h>
#include <core/file_utils.h>
#include <core/json.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/progress.h>

#include <GLFW/glfw3.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <thread>
#include <mutex>
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
    std::mutex g_platformMutex;
    uint32_t g_platformUsers = 0;

    void AcquireAppPlatform()
    {
        std::lock_guard lock(g_platformMutex);
        if (g_platformUsers++ == 0)
            caustica::initializeAppPlatform();
    }

    void ReleaseAppPlatform()
    {
        std::lock_guard lock(g_platformMutex);
        if (g_platformUsers > 0 && --g_platformUsers == 0)
            caustica::shutdownAppPlatform();
    }

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

        return caustica::getDirectoryWithExecutable();
    }

    std::filesystem::path ResolveRuntimeDirectory()
    {
        std::filesystem::path moduleDirectory = GetCurrentModuleDirectory();
        if (std::filesystem::exists(moduleDirectory / "ShaderBin"))
            return moduleDirectory;

        std::filesystem::path executableDirectory = caustica::getDirectoryWithExecutable();
        if (std::filesystem::exists(executableDirectory / "ShaderBin"))
            return executableDirectory;

        return moduleDirectory;
    }

    std::filesystem::path ResolveResourceRoot(const std::filesystem::path& runtimeDirectory)
    {
        if (std::filesystem::exists(runtimeDirectory / caustica::c_AssetsFolder))
            return runtimeDirectory;

        std::filesystem::path parentDirectory = runtimeDirectory.parent_path();
        if (std::filesystem::exists(parentDirectory / caustica::c_AssetsFolder))
            return parentDirectory;

        return caustica::getDirectoryWithExecutable();
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
        settings["name"] = "SceneSettings";
        settings["type"] = "SceneSettings";
        settings["realtimeMode"] = true;
        settings["startingCamera"] = -1;
        root["graph"].append(settings);

        return caustica::json::toString(root);
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

    caustica::rhi::GraphicsAPI ResolveGraphicsAPI(const RenderSession::Config& cfg)
    {
#if CAUSTICA_WITH_DX12 && CAUSTICA_WITH_VULKAN
        return cfg.useVulkan ? caustica::rhi::GraphicsAPI::VULKAN : caustica::rhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
        if (!cfg.useVulkan)
            caustica::warning("RenderSession: DX12 was requested but this build only has Vulkan; using Vulkan.");
        return caustica::rhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX12
        if (cfg.useVulkan)
            caustica::warning("RenderSession: Vulkan was requested but this build only has DX12; using DX12.");
        return caustica::rhi::GraphicsAPI::D3D12;
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
    AcquireAppPlatform();
    m_platformInitialized = true;
    m_config.scene = PrepareSceneArgument(cfg.scene);

    if (cfg.nonInteractive)
    {
        caustica::enableOutputToMessageBox(false);
        caustica::enableOutputToConsole(true);
        caustica::setMinSeverity(caustica::Severity::Warning);
        caustica::helpersSetNonInteractive();
    }

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (!cfg.useVulkan)
        m_d3d12DeviceFactory = CreateD3D12AgilityDeviceFactory();
#endif

    caustica::EngineAppDesc desc{};
    desc.width = uint32_t(cfg.width);
    desc.height = uint32_t(cfg.height);
    desc.headless = cfg.headless;
    desc.debugDevice = cfg.debug;
    desc.adapterIndex = cfg.adapterIndex;
    desc.useVulkan = cfg.useVulkan;
    desc.scene = m_config.scene;
    desc.windowTitle = "caustica_py";
    desc.dedicatedRenderThread = !cfg.headless;
    desc.runtimeDirectory = ResolveRuntimeDirectory();
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (m_d3d12DeviceFactory)
        desc.d3d12DeviceFactory = m_d3d12DeviceFactory.Get();
#endif

    m_engine = caustica::EngineApp::create(std::move(desc));
    if (!m_engine || !m_engine->isValid())
    {
        caustica::error("RenderSession: failed to initialize EngineApp");
        return;
    }

    auto& cmdLine = m_engine->commandLine();
    cmdLine.nonInteractive = cfg.nonInteractive;
    cmdLine.OverrideToReferenceMode = !cfg.realtimeMode;
    cmdLine.OverrideToRealtimeMode = cfg.realtimeMode;
    cmdLine.ReferenceSamplesPerPixel = cfg.accumulationTarget;
    m_engine->renderAppState().settings.AccumulationTarget = cfg.accumulationTarget;

    m_engine->app().beforePresent = [this](caustica::GpuDevice& manager, uint32_t) {
        m_lastRenderedBackBufferIndex = manager.getCurrentBackBufferIndex();
    };

    m_initialized = true;

    if (!m_config.scene.empty() && !WaitUntilReady())
    {
        caustica::error("RenderSession: scene did not become ready");
        shutdown();
    }
}

RenderSession::~RenderSession()
{
    shutdown();
}

void RenderSession::shutdown()
{
    if (m_engine)
        m_engine->shutdown();
    m_engine.reset();
    m_initialized = false;
    if (m_platformInitialized)
    {
        ReleaseAppPlatform();
        m_platformInitialized = false;
    }
}

bool RenderSession::LoadScene(const std::string& sceneName, bool waitUntilReady,
                              double timeoutSeconds, int warmupFrames)
{
    if (!m_initialized || !m_engine)
        return false;

    m_engine->setScene(PrepareSceneArgument(sceneName), /*forceReload=*/true);

    if (waitUntilReady)
        return WaitUntilReady(timeoutSeconds, warmupFrames);
    return true;
}

bool RenderSession::IsSceneReady() const
{
    if (!m_initialized || !m_engine || !m_engine->isSceneLoaded())
        return false;

    const auto* viewState = m_engine->app().tryResource<caustica::SceneViewState>();
    if (!viewState)
        return !m_engine->isSceneLoading();

    // Secondary opacity/OMM streaming may continue after the scene is already
    // published and renderable. Prewarm only needs the primary load transaction
    // to finish and rendering suspension to be released.
    return !viewState->loadSession.isActive()
        && !viewState->sceneGpuSuspended.load(std::memory_order_acquire);
}

bool RenderSession::WaitUntilReady(double timeoutSeconds, int warmupFrames)
{
    if (!m_initialized || !m_engine)
        return false;

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    while (timeoutSeconds <= 0.0 || std::chrono::steady_clock::now() - start < timeout)
    {
        if (!Step(0.0f))
            return false;
        if (IsSceneReady())
        {
            if (warmupFrames > 0 && !StepN(warmupFrames))
                return false;
            m_engine->renderAppState().settings.ResetAccumulation = true;
            return true;
        }
        // Scene import and GPU streaming run on worker/render domains. A headless
        // loop can otherwise consume the frame budget in a fraction of a second
        // before those domains get enough wall-clock time to finish.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto* viewState = m_engine->app().tryResource<caustica::SceneViewState>();
    const auto* manager = caustica::detail::sessionManager(m_engine->app());
    caustica::warning(
        "RenderSession: scene did not become ready within %.1f seconds "
        "(phase=%s managerLoaded=%d managerLoading=%d gpuSuspended=%d secondaryStreaming=%d)",
        timeoutSeconds,
        viewState ? caustica::loadSessionPhaseName(viewState->loadSession.phase) : "Unavailable",
        manager && manager->isSceneLoaded() ? 1 : 0,
        manager && manager->isSceneLoading() ? 1 : 0,
        viewState && viewState->sceneGpuSuspended.load(std::memory_order_acquire) ? 1 : 0,
        viewState && viewState->loadSession.secondaryStreaming.load(std::memory_order_acquire) ? 1 : 0);
    return false;
}

bool RenderSession::Step(float dt)
{
    if (!m_initialized || !m_engine)
        return false;

    auto* device = m_engine->device();
    if (!device)
        return false;

    const bool frameOk = dt >= 0.0f
        ? m_engine->stepFrame(dt)
        : m_engine->stepFrame(m_config.headless ? float(c_HeadlessFrameTimeSeconds) : -1.f);

    if (!frameOk)
        return false;

    // Headless Python stepping can outrun the GPU and cause resource hazards
    // (e.g. screenshot readback or auto-exposure buffer maps). Serialize frames.
    if (m_config.headless)
    {
        if (!device->getDevice()->waitForIdle())
        {
            caustica::error("RenderSession: GPU device lost or removed");
            return false;
        }
    }

    GLFWwindow* window = device->getWindow();
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
    if (!m_initialized || !m_engine)
        return 0;

    // Force reference / accumulation mode so we know "done" actually means
    // the SPP target has been reached.
    auto& settings = m_engine->renderAppState().settings;
    settings.ResetAccumulation = true;

    int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, settings.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        // All samples of one reference output frame must observe the same
        // simulation/animation time. Sequence callers advance the pose once,
        // then this loop freezes time while accumulating samples.
        if (!Step(0.0f)) break;
        ++frames;
        if (m_engine->accumulationCompleted())
            break;
    }
    return frames;
}

bool RenderSession::PrepareAnimationFrame(
    double sceneTime,
    bool importedAnimations,
    bool keyframes)
{
    if (!m_initialized || !m_engine || !std::isfinite(sceneTime))
        return false;

    caustica::App& app = m_engine->app();
    auto& settings = m_engine->renderAppState().settings;
    const bool previousRealtime = settings.RealtimeMode;
    const bool previousAnimations = settings.EnableAnimations;
    const bool previousKeyframes = settings.EnableKeyframes;

    // Animation evaluation currently belongs to the realtime update path.
    // Evaluate exactly once at the requested clock, then restore the caller's
    // mode so reference accumulation can sample a frozen pose.
    caustica::setSceneTime(app, sceneTime);
    if (auto* viewState = app.tryResource<caustica::SceneViewState>())
        viewState->keyframeTime = sceneTime;
    settings.RealtimeMode = true;
    settings.EnableAnimations = importedAnimations;
    settings.EnableKeyframes = keyframes;
    settings.ResetRealtimeCaches = true;
    const bool ok = Step(0.0f);

    settings.RealtimeMode = previousRealtime;
    settings.EnableAnimations = previousAnimations;
    settings.EnableKeyframes = previousKeyframes;
    settings.ResetAccumulation = true;
    return ok;
}

int RenderSession::RenderReferenceFrame(int spp, bool oidn, int maxFrames)
{
    if (!m_initialized || !m_engine || spp <= 0)
        return 0;

    auto& settings = m_engine->renderAppState().settings;
    settings.RealtimeMode = false;
    settings.AccumulationTarget = spp;
    settings.AccumulationPreWarmRealtimeCaches = false;
    settings.ReferenceOIDNDenoiser = oidn;
    settings.ReferenceOIDNDenoiserChanged = true;
    return StepUntilAccumulated(maxFrames);
}

bool RenderSession::RenderRealtimeFrame(float dt)
{
    if (!m_initialized || !m_engine || !std::isfinite(dt) || dt < 0.0f)
        return false;

    auto& settings = m_engine->renderAppState().settings;
    if (!settings.RealtimeMode)
    {
        settings.RealtimeMode = true;
        settings.ResetAccumulation = true;
        settings.ResetRealtimeCaches = true;
    }
    settings.AccumulationTarget = 1;
    return Step(dt);
}

bool RenderSession::SaveScreenshot(const std::string& outputPath)
{
    if (!m_initialized || !m_engine)
        return false;

    auto* device = m_engine->device();
    if (!device)
        return false;

    caustica::rhi::Texture* tex = m_engine->ldrColorTexture();
    caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::ShaderResource;

    if (!tex)
    {
        uint32_t backBufferIndex = m_lastRenderedBackBufferIndex;
        if (backBufferIndex == UINT32_MAX)
            backBufferIndex = device->getCurrentBackBufferIndex();

        tex = device->getBackBuffer(backBufferIndex);
        state = m_config.headless
            ? caustica::rhi::ResourceStates::RenderTarget
            : caustica::rhi::ResourceStates::Present;
    }

    if (!tex)
    {
        caustica::error("RenderSession: no current output texture");
        return false;
    }

    auto* infra = caustica::gpuSharedCaches(m_engine->app());
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        caustica::error("RenderSession: render device not initialized yet");
        return false;
    }

    // saveTextureToFile creates its own command list. wait for the last rendered
    // frame to finish so LdrColor is not still in use by an in-flight submit.
    if (!device->getDevice()->waitForIdle())
    {
        caustica::error("RenderSession: GPU device lost or removed before screenshot");
        return false;
    }

    std::filesystem::path p(outputPath);
    if (p.has_parent_path())
        caustica::ensureDirectoryExists(p.parent_path());

    return caustica::saveTextureToFile(
        device->getDevice(),
        *renderDevice,
        tex,
        state,
        outputPath.c_str());
}

std::optional<RenderSession::FramebufferLdr> RenderSession::GetFramebufferLdr()
{
    if (!m_initialized || !m_engine)
        return std::nullopt;

    auto* gpuDevice = m_engine->device();
    if (!gpuDevice)
        return std::nullopt;

    caustica::rhi::Device* device = gpuDevice->getDevice();
    if (!device)
        return std::nullopt;

    caustica::rhi::Texture* texture = m_engine->ldrColorTexture();
    caustica::rhi::ResourceStates textureState = caustica::rhi::ResourceStates::ShaderResource;

    if (!texture)
    {
        uint32_t backBufferIndex = m_lastRenderedBackBufferIndex;
        if (backBufferIndex == UINT32_MAX)
            backBufferIndex = gpuDevice->getCurrentBackBufferIndex();

        texture = gpuDevice->getBackBuffer(backBufferIndex);
        textureState = m_config.headless
            ? caustica::rhi::ResourceStates::RenderTarget
            : caustica::rhi::ResourceStates::Present;
    }

    if (!texture)
    {
        caustica::error("RenderSession: no current output texture for framebuffer readback");
        return std::nullopt;
    }

    auto* infra = caustica::gpuSharedCaches(m_engine->app());
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        caustica::error("RenderSession: render device not initialized yet");
        return std::nullopt;
    }

    if (!device->waitForIdle())
    {
        caustica::error("RenderSession: GPU device lost or removed before framebuffer readback");
        return std::nullopt;
    }

    // Mirror saveTextureToFile: blit non-RGBA8 targets to SRGBA8, then staging copy.
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
    stagingDesc.debugName = "GetFramebufferLdr Staging";

    caustica::rhi::StagingTextureHandle stagingTexture = device->createStagingTexture(stagingDesc, caustica::rhi::CpuAccessMode::Read);
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

    FramebufferLdr result;
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

bool RenderSession::SetCamera(const caustica::math::float3& pos,
                              const caustica::math::float3& dir,
                              const caustica::math::float3& up)
{
    if (!m_engine) return false;

    auto v3 = [](const caustica::math::float3& v) {
        return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
    };
    std::string s = v3(pos) + "," + v3(dir) + "," + v3(up);
    return m_engine->setCameraPosDirUp(s);
}

void RenderSession::SetCameraFOV(float verticalFovDegrees)
{
    if (m_engine)
        m_engine->setCameraVerticalFOV(caustica::math::radians(verticalFovDegrees));
}

void RenderSession::setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (m_engine)
        m_engine->setCameraIntrinsics(fx, fy, cx, cy, width, height);
}

#endif // CAUSTICA_WITH_PYTHON
