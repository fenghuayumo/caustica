#include "EditorApplication.h"

#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <events/application_event.h>

#include <imgui/imgui_renderer.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <render/Passes/Debug/Korgi.h>
#include <SampleUI.h>
#include "SampleCommon/LocalConfig.h"
#include <assets/loader/ShaderPackFileSystem.h>

#include <backend/ShaderUtils.h>
#include <core/path_utils.h>
#include <platform/cmdline_utils.h>
#include <core/log.h>
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif
#include "AdvancedPathTracer.h"
#include "PathTracerApp.h"
#include "input/PathTracerInputController.h"
#include <render/WorldRenderer/PathTracingWorldRenderer.h>

#include <core/vfs/VFS.h>
#include <render/Core/BindingCache.h>

#include <GLFW/glfw3.h>
#include <platform/glfw_window.h>
#include <platform/window.h>

extern const char* g_windowTitle;

#if CAUSTICA_D3D12_WITH_NVAPI
#include <nvApi.h>

#if 0
// Validation callback
static void __stdcall myValidationMessageCallback(void* pUserData, NVAPI_D3D12_RAYTRACING_VALIDATION_MESSAGE_SEVERITY severity, const char* messageCode, const char* message, const char* messageDetails)
{
    const char* severityString = "unknown";
    switch (severity)
    {
    case NVAPI_D3D12_RAYTRACING_VALIDATION_MESSAGE_SEVERITY_ERROR: severityString = "error"; break;
    case NVAPI_D3D12_RAYTRACING_VALIDATION_MESSAGE_SEVERITY_WARNING: severityString = "warning"; break;
    }

    caustica::warning("NVAPI Ray Tracing Validation message: %s: [%s] %s\n%s", severityString, messageCode, message, messageDetails);
}
#endif
#endif

namespace
{
    std::string LowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }

    bool IsTrueOptionValue(const std::string& value)
    {
        return value.empty()
            || value == "1"
            || value == "true"
            || value == "yes"
            || value == "on";
    }

    bool CommandLineWantsConsoleLogging(int argc, char const* const* argv)
    {
        for (int n = 1; n < argc; ++n)
        {
            std::string arg = LowerAscii(argv[n] ? argv[n] : "");
            if (arg.rfind("--", 0) == 0)
                arg.erase(0, 2);
            else if (!arg.empty() && (arg[0] == '-' || arg[0] == '/'))
                arg.erase(0, 1);

            const size_t equals = arg.find('=');
            const std::string key = arg.substr(0, equals);
            const std::string value = (equals == std::string::npos) ? "" : arg.substr(equals + 1);
            if ((key == "nowindow" || key == "noninteractive") && IsTrueOptionValue(value))
                return true;
        }

        return false;
    }

    bool TryParseBackendName(const std::string& value, nvrhi::GraphicsAPI& api)
    {
        const std::string backend = LowerAscii(value);
        if (backend == "vk" || backend == "vulkan")
        {
            api = nvrhi::GraphicsAPI::VULKAN;
            return true;
        }
        if (backend == "dx12" || backend == "d3d12" || backend == "directx12" || backend == "directx")
        {
            api = nvrhi::GraphicsAPI::D3D12;
            return true;
        }
        return false;
    }

    void AppendUnique(std::vector<std::string>& values, const std::string& value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }

    nvrhi::GraphicsAPI GetRtxptGraphicsAPIFromCommandLine(int argc, const char* const* argv)
    {
#if defined(_WIN32)
        nvrhi::GraphicsAPI api = caustica::GetGraphicsAPIFromCommandLine(argc, argv);

        for (int n = 1; n < argc; ++n)
        {
            std::string arg = argv[n] ? argv[n] : "";
            std::string key = arg;
            std::string value;

            const size_t equals = key.find('=');
            if (equals != std::string::npos)
            {
                value = LowerAscii(key.substr(equals + 1));
                key = key.substr(0, equals);
            }

            key = LowerAscii(key);

            if (key == "-vk" || key == "--vk" || key == "-vulkan" || key == "--vulkan")
            {
                if (IsTrueOptionValue(value))
                    api = nvrhi::GraphicsAPI::VULKAN;
            }
            else if (key == "-d3d12" || key == "--d3d12" || key == "-dx12" || key == "--dx12")
            {
                if (IsTrueOptionValue(value))
                    api = nvrhi::GraphicsAPI::D3D12;
            }
            else if (key == "--backend" || key == "--api" || key == "--graphicsapi")
            {
                std::string backend = value;
                if (backend.empty() && n + 1 < argc)
                    backend = argv[++n] ? argv[n] : "";

                nvrhi::GraphicsAPI parsedApi;
                if (TryParseBackendName(backend, parsedApi))
                    api = parsedApi;
                else
                    caustica::warning("Unknown render backend '%s'. Falling back to the default backend.", backend.c_str());
            }
        }

        return api;
#else
        return nvrhi::GraphicsAPI::VULKAN;
#endif
    }
}


EditorApplication::EditorApplication()
{
    RegisterLogCallback();
    korgi::Init();
}

EditorApplication::~EditorApplication()
{
    korgi::Shutdown();
}

EditorApplication::StartupResult EditorApplication::startup(int argc, const char* const* argv)
{
#if defined(_WIN32)
    nvrhi::GraphicsAPI api = GetRtxptGraphicsAPIFromCommandLine(argc, argv);
#else
    nvrhi::GraphicsAPI api = GetRtxptGraphicsAPIFromCommandLine(0, nullptr);
#endif
    m_GpuDevice = std::unique_ptr<caustica::GpuDevice>(caustica::GpuDevice::Create(api));
    m_GpuDevice->SetFrameTimeUpdateInterval(1.0f);

    caustica::DeviceCreationParameters deviceParams = GetDefaultDeviceParams();

    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    // Process command line arguments
    if (!ProcessCommandLine(argc, argv, deviceParams, preferredScene))
    {
        return StartupResult::FailProcessingCommandLine;
    }

    if (!InitDeviceAndWindow(deviceParams))
    {
        return StartupResult::FailToCreateDevice;
    }

    bindFrameDriver(m_GpuDevice.get());

    // Check API feature support
    if (!CheckDeviceFeatureSupport(deviceParams))
    {
        return StartupResult::FailDeviceFeatureSupport;
    }

    CreateShaderFactory();

    m_scenePass = std::make_unique<AdvancedPathTracer>(*m_GpuDevice, m_CmdLine, m_sampleUIData);
    initRenderInfrastructurePhase1();

    const nvrhi::BindingLayoutHandle bindlessLayout =
        caustica::render::PathTracingWorldRenderer::CreateBindlessLayout(m_GpuDevice->GetDevice());
    initRenderInfrastructurePhase2(bindlessLayout);
    initSceneServices();
    initWorldRenderer(bindlessLayout);

    m_scenePass->Init(preferredScene, m_ShaderFactory);
    syncPassesToBackBuffer();

#if CAUSTICA_WITH_DX12 && (CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619)   // temporary
    // When using AgilitySDK >= 619, we require shader model 6.9
    if (m_GpuDevice->GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        ID3D12Device* d3d12Device = static_cast<ID3D12Device*>(
            m_GpuDevice->GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D12_Device)
            );

        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_9 };

        HRESULT hr = d3d12Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
        assert(SUCCEEDED(hr));
        if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_9)
        {
            caustica::fatal("Shader Model 6.9 is required when compiled with Agility SDK 1.619 or newer, but is unsupported on the current device. Please check for newer graphics drivers, or recompile without Agility SDK");
            return StartupResult::FailToCreateDevice;
        }
    }
#endif

    // Optionally create the UP render pass. This exposes run time parameter controls.
    if (!m_CmdLine.noWindow)
    {
        m_uiPass = std::make_unique<SampleUI>(m_GpuDevice.get(), *this, *m_scenePass, m_sampleUIData, IsSERSupported(), m_CmdLine);
        m_uiPass->Init(m_ShaderFactory);
        syncPassesToBackBuffer();
    }
    else
    {
        InitializeSampleUIDataFromCommandLine(m_sampleUIData, m_CmdLine);
    }

    // Register file drag-and-drop without touching GLFW's window user pointer —
    // GlfwWindow stores that pointer and all other callbacks depend on it.
    if (!m_CmdLine.noWindow && m_GpuDevice->GetPlatformWindow())
    {
        m_GpuDevice->GetPlatformWindow()->setFileDropCallback(
            [this](int count, const char** paths)
            {
                for (int i = 0; i < count; ++i)
                    m_sampleUIData.PendingDroppedFiles.emplace_back(paths[i]);
            });

        // Install the Application ↔ Window event bridge so keyboard/mouse/window
        // events flow through the event system (both for run() and stepFrame() paths).
        m_GpuDevice->GetPlatformWindow()->setEventCallback(
            [this](caustica::Event& e) { this->onWindowEvent(e); });
    }

    LocalConfig::PostAppInit(m_sampleUIData);

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    auto device = m_GpuDevice->GetDevice();
    if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        ID3D12Device* d3dDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

        LUID luid = d3dDevice->GetAdapterLuid();
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&m_d3dAdapter));
        //m_d3dAdapter->QueryVideoMemoryInfo()
    }
#endif

    return StartupResult::Success;
}

bool EditorApplication::QueryVideoMemoryInfo(uint64_t& outBudget, uint64_t& outCurrentUsage, uint64_t& outAvailableForReservation, uint64_t& outCurrentReservation)
{
#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    DXGI_QUERY_VIDEO_MEMORY_INFO info;
    if (FAILED(m_d3dAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    outBudget = info.Budget;
    outCurrentUsage = info.CurrentUsage;
    outAvailableForReservation = info.AvailableForReservation;
    outCurrentReservation = info.CurrentReservation;
    return true;
#else
    return false;
#endif
}

void EditorApplication::shutdown()
{
    if (m_shutdownCalled)
        return;

    unbindFrameDriver(m_GpuDevice.get());
    m_uiPass.reset();

#if CAUSTICA_D3D12_WITH_NVAPI
    if (m_NVAPIValidationHandle != nullptr)
    {
        auto nativeObj = m_GpuDevice->GetDevice()->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        NvAPI_D3D12_FlushRaytracingValidationMessages(nativeObj);
    }
#endif

    if (m_worldRenderer)
        m_worldRenderer.reset();

    if (m_scenePass)
        m_scenePass.reset();

    m_sceneManager.reset();
    m_renderCore.reset();

    m_textureCache.reset();
    m_descriptorTable.reset();
    m_bindingCache.reset();
    m_commonPasses.reset();

    m_ShaderFactory.reset();

    if (m_GpuDevice)
        m_GpuDevice->ReleaseWindowOwnership();

    m_Window.reset();

    if (m_GpuDevice)
    {
        m_GpuDevice->Shutdown();
        m_GpuDevice.reset();
    }

    Application::shutdown();
}

void EditorApplication::RegisterLogCallback()
{
    // Get the default call back first so we can pass messages through to it.
    m_DefaultLogCallback = caustica::GetCallback();

    // Register our custom callback to intercept and filter streamline errors
    caustica::SetCallback([this](caustica::Severity severity, const char* message)
        {
            this->SampleLogCallback(severity, message);
        });
}

void EditorApplication::SampleLogCallback(caustica::Severity severity, const char* message)
{
    // This lets us demote some of Streamline errors that aren't errors into warnings
    if (severity == caustica::Severity::Error)
    {
        std::string msg(message);
        if (msg.find("Don't know the size") != std::string::npos)
            severity = caustica::Severity::Warning;
        if (msg.find("dlss_gEntry.cpp") != std::string::npos)
        {
            if (msg.find("Unable to find DRS context") != std::string::npos
                || msg.find("NGX indicates DLSS-G is not available") != std::string::npos)
                severity = caustica::Severity::Warning;
        }
        if (msg.find("Missing NGX context") != std::string::npos
            || msg.find("Unable to find NGX ") != std::string::npos
            || msg.find("NvAPI_D3D_Sleep") != std::string::npos)
            severity = caustica::Severity::Warning;
    }

    // Pass all other messages to the default log callback
    m_DefaultLogCallback(severity, message);
}

caustica::DeviceCreationParameters EditorApplication::GetDefaultDeviceParams() const
{
    caustica::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 0;   // initialized from CmdLine
    deviceParams.backBufferHeight = 0;  // initialized from CmdLine
    deviceParams.swapChainSampleCount = 1;
    deviceParams.swapChainBufferCount = c_SwapchainCount;
    deviceParams.startFullscreen = false;
    deviceParams.startBorderless = false;
    deviceParams.vsyncEnabled = true;
    deviceParams.enableRayTracingExtensions = true;
#if CAUSTICA_WITH_DX12
#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    deviceParams.featureLevel = D3D_FEATURE_LEVEL_12_2;
    // TODO: Redefining this isn't needed. Take the ones from AgilitySDK
    static const UUID D3D12ExperimentalShaderModels = { 0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f} };
    static const UUID D3D12StateObjectsExperiment = { 0x398a7fd6, 0xa15a, 0x42c1, {0x96, 0x05, 0x4b, 0xd9, 0x99, 0x9a, 0x61, 0xaf} };
    static const UUID D3D12RaytracingExperiment = { 0xb56e238b, 0xe886, 0x46d8, {0x9b, 0xe1, 0x34, 0x10, 0x30, 0x31, 0x45, 0x09} };
    UUID Features[] = { D3D12ExperimentalShaderModels }; //, D3D12StateObjectsExperiment }; //, D3D12RaytracingExperiment };
    HRESULT ok = D3D12EnableExperimentalFeatures(_countof(Features), Features, nullptr, nullptr);
#else
    deviceParams.featureLevel = D3D_FEATURE_LEVEL_12_1;
#endif
#endif
#if defined(_DEBUG)
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableWarningsAsErrors = true;
    deviceParams.enableNvrhiValidationLayer = true;
    deviceParams.enableGPUValidation = false;       // <- this severely impact performance but is good to enable from time to time
#endif
    deviceParams.supportExplicitDisplayScaling = true;

#if CAUSTICA_WITH_STREAMLINE
    deviceParams.checkStreamlineSignature = true;   // <- Set to false if you're using a local build of streamline
    deviceParams.streamlineAppId = 231313132;
#if defined(_DEBUG)
    deviceParams.enableStreamlineLog = true;
#endif
#endif

#if CAUSTICA_WITH_VULKAN
#if CAUSTICA_WITH_NATIVE_DLSS
    caustica::render::DLSS::GetRequiredVulkanExtensions(
        deviceParams.requiredVulkanInstanceExtensions,
        deviceParams.requiredVulkanDeviceExtensions);
#endif
    AppendUnique(deviceParams.requiredVulkanDeviceExtensions, "VK_KHR_buffer_device_address");
    AppendUnique(deviceParams.requiredVulkanDeviceExtensions, "VK_KHR_format_feature_flags2");

    // Attachment 0 not written by fragment shader; undefined values will be written to attachment (OMM baker)
    deviceParams.ignoredVulkanValidationMessageLocations.push_back(0x0000000023e43bb7);

    // vertex shader writes to output location 0.0 which is not consumed by fragment shader (OMM baker)
    deviceParams.ignoredVulkanValidationMessageLocations.push_back(0x000000000609a13b);

    // vkCmdDispatch(): Buffer format mismatch for t_NeighborOffsets (RG8_SNORM vs shader expecting RG32_FLOAT)
    // DX12 handles this automatically; it works correctly, just Vulkan is stricter about format matching
    deviceParams.ignoredVulkanValidationMessageLocations.push_back(0x00000000c5a3822a);

    // vkCmdPipelineBarrier2(): pDependencyInfo.pBufferMemoryBarriers[0].dstAccessMask bit VK_ACCESS_SHADER_READ_BIT
    // is not supported by stage mask (Unhandled VkPipelineStageFlagBits)
    // Vulkan validation layer not supporting OMM?
    deviceParams.ignoredVulkanValidationMessageLocations.push_back(0x00000000591f70f2);

    // vkCmdPipelineBarrier2(): pDependencyInfo->pBufferMemoryBarriers[0].dstAccessMask(VK_ACCESS_SHADER_READ_BIT) is not supported by stage mask(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT)
    // Vulkan Validaiotn layer not supporting OMM bug
    deviceParams.ignoredVulkanValidationMessageLocations.push_back(0x000000005e6e827d);
#endif

    deviceParams.enablePerMonitorDPI = true;

    return deviceParams;
}

bool EditorApplication::ProcessCommandLine(int argc, char const* const* argv,
    caustica::DeviceCreationParameters& deviceParams, std::string& preferredScene)
{
#if 1 // use a bit larger window by default if screen large enough
    glfwInit();
    const auto primMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = (primMonitor != nullptr) ? glfwGetVideoMode(primMonitor) : (nullptr);
    if (mode->width > 2560 && mode->height > 1440)
    {
        m_CmdLine.width = 2560;
        m_CmdLine.height = 1440;
    }
#endif
    if (CommandLineWantsConsoleLogging(argc, argv))
        caustica::ConsoleApplicationMode();

    if (!m_CmdLine.InitFromCommandLine(argc, argv))
    {
        return false;
    }

    if (!m_CmdLine.scene.empty())
    {
        preferredScene = m_CmdLine.scene;
    }

    if (m_CmdLine.noWindow)
        m_CmdLine.nonInteractive = true;

    if (m_CmdLine.nonInteractive)
    {   
        caustica::EnableOutputToMessageBox(false);
        HelpersSetNonInteractive();
    }
    if (m_CmdLine.noWindow || m_CmdLine.nonInteractive)
    {
        caustica::ConsoleApplicationMode();
    }

    if (m_CmdLine.debug)
    {
        deviceParams.enableDebugRuntime = true;
        deviceParams.enableNvrhiValidationLayer = true;
    }

    deviceParams.backBufferWidth = m_CmdLine.width;
    deviceParams.backBufferHeight = m_CmdLine.height;
    deviceParams.startFullscreen = m_CmdLine.fullscreen;
    deviceParams.adapterIndex = m_CmdLine.adapterIndex;

    return true;
}

bool EditorApplication::InitDeviceAndWindow(const caustica::DeviceCreationParameters& deviceParams)
{
    if (m_CmdLine.noWindow)
    {
        if (!m_GpuDevice->CreateHeadlessDevice(deviceParams))
        {
            caustica::fatal("CreateHeadlessDevice failed: Cannot initialize a graphics device with the requested parameters");
            return false;
        }
    }
    else
    {
        // --- Platform layer: create Window via GlfwWindow ---
        caustica::GlfwWindow::makeDefault();

        caustica::WindowDesc wDesc;
        wDesc.Width      = deviceParams.backBufferWidth;
        wDesc.Height     = deviceParams.backBufferHeight;
        wDesc.Fullscreen = deviceParams.startFullscreen;
        wDesc.Borderless = deviceParams.startBorderless;
        wDesc.VSync      = deviceParams.vsyncEnabled;
        wDesc.Title      = g_windowTitle ? g_windowTitle : "caustica";
        wDesc.RenderAPI  = static_cast<int>(m_GpuDevice->GetGraphicsAPI());

        m_Window.reset(caustica::Window::create(wDesc));
        if (!m_Window || !m_Window->hasInitialised())
        {
            caustica::fatal("Cannot create window via platform layer");
            return false;
        }

        // --- Backend layer: GPU device + swapchain via GpuDevice ---
        if (!m_GpuDevice->CreateDeviceAndSwapChain(deviceParams, m_Window.get()))
        {
            caustica::fatal("Cannot initialize a graphics device with the requested parameters");
            return false;
        }

        // --- Engine layer: device + window ready for run() ---
        HelpersRegisterActiveWindow();
    }

#if 0 && CAUSTICA_D3D12_WITH_NVAPI
    static bool NVAPI_VALIDATION = false;
    auto device = m_GpuDevice->GetDevice();
    if (NVAPI_VALIDATION && device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        NvAPI_Status res;
        auto nativeObj = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        //res = NvAPI_D3D12_EnableRaytracingValidation(nativeObj, NVAPI_D3D12_RAYTRACING_VALIDATION_FLAG_NONE);
        res = NvAPI_D3D12_RegisterRaytracingValidationMessageCallback(nativeObj, &myValidationMessageCallback, (void*)this, &this->m_NVAPIValidationHandle);
        assert( res == NvAPI_Status:: NVAPI_OK );
    }
#endif

    return true;
}

bool EditorApplication::CheckDeviceFeatureSupport(const caustica::DeviceCreationParameters& deviceParams)
{
    auto device = m_GpuDevice->GetDevice();
    if (!device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
    {
        caustica::fatal("The graphics device does not support Ray Tracing Pipelines");
        return false;
    }

    if (!device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        caustica::fatal("The graphics device does not support Ray Queries");
        return false;
    }

    return true;
}

void EditorApplication::initRenderInfrastructurePhase1()
{
    auto device = m_GpuDevice->GetDevice();
    m_commonPasses = std::make_shared<caustica::CommonRenderPasses>(device, m_ShaderFactory);
    m_bindingCache = std::make_unique<caustica::BindingCache>(device);

    m_scenePass->AttachRenderResources(m_ShaderFactory, m_commonPasses, m_bindingCache.get(), m_descriptorTable, m_textureCache);
}

void EditorApplication::initRenderInfrastructurePhase2(nvrhi::IBindingLayout* bindlessLayout)
{
    auto device = m_GpuDevice->GetDevice();
    m_descriptorTable = std::make_shared<caustica::DescriptorTableManager>(device, bindlessLayout);

    auto nativeFS = std::make_shared<caustica::NativeFileSystem>();
    m_textureCache = std::make_shared<caustica::TextureCache>(device, nativeFS, m_descriptorTable);

    m_scenePass->AttachRenderResources(m_ShaderFactory, m_commonPasses, m_bindingCache.get(), m_descriptorTable, m_textureCache);
}

caustica::render::WorldRendererServices EditorApplication::buildWorldRendererServices()
{
    PathTracerApp& editor = *m_scenePass;
    return caustica::render::WorldRendererServices{
        .gpuDevice = *m_GpuDevice,
        .sceneManager = *m_sceneManager,
        .renderCore = *m_renderCore,
        .settings = editor.GetPathTracerSettings(),
        .shaderFactory = m_ShaderFactory,
        .commonPasses = m_commonPasses,
        .bindingCache = *m_bindingCache,
        .textureCache = m_textureCache,
        .descriptorTable = m_descriptorTable,
        .envMapBaker = editor.GetEnvMapBaker(),
        .lightsBaker = editor.GetLightsBaker(),
        .materialsBaker = editor.GetMaterialsBaker(),
        .ommBaker = editor.GetOMMBaker(),
        .computePipelineBaker = editor.GetComputePipelineBaker(),
        .lights = editor.GetLights(),
        .envMapSceneParams = editor.GetEnvMapSceneParams(),
        .envMapLocalPath = editor.GetEnvMapLocalPath(),
        .envMapOverride = editor.GetEnvMapOverrideSource(),
        .sceneTime = editor.GetSceneTimeRef(),
        .gaussianSplatEmissionProxies = editor.GetGaussianSplatEmissionProxies(),
        .progressInitializingRenderer = editor.GetProgressInitializingRenderer(),
        .asyncLoadingInProgress = editor.GetAsyncLoadingInProgressRef(),
        .benchStart = editor.GetBenchStart(),
        .benchLast = editor.GetBenchLast(),
        .benchFrames = editor.GetBenchFrames(),
    };
}

void EditorApplication::initWorldRenderer(nvrhi::IBindingLayout* bindlessLayout)
{
    m_worldRendererServices = std::make_unique<caustica::render::WorldRendererServices>(buildWorldRendererServices());
    m_worldRenderer = std::make_unique<caustica::render::PathTracingWorldRenderer>(*m_worldRendererServices, *m_scenePass);
    m_scenePass->AttachWorldRenderer(m_worldRenderer.get());
    m_worldRenderer->createBindingLayouts(bindlessLayout);
}

void EditorApplication::initSceneServices()
{
    m_renderCore = std::make_unique<caustica::RenderCore>(m_GpuDevice->GetDevice());
    m_renderCore->camera().camera().SetRotateSpeed(.003f);

    m_sceneManager = std::make_unique<SceneManager>(
        *m_GpuDevice,
        *m_ShaderFactory,
        m_textureCache,
        m_descriptorTable);

    m_scenePass->AttachSceneServices(*m_sceneManager, *m_renderCore);

    m_sceneManager->setLoadingCallbacks(
        [this]()
        {
            if (m_scenePass)
                m_scenePass->SceneLoaded();
        },
        [this]()
        {
            if (m_scenePass)
                m_scenePass->SceneUnloading();
        });

    m_renderCore->initializeRenderPipeline(m_ShaderFactory);
}

void EditorApplication::CreateShaderFactory()
{
    const char* shaderTypeName = caustica::GetShaderTypeName(m_GpuDevice->GetGraphicsAPI());
    const std::filesystem::path appDirectory = caustica::GetDirectoryWithExecutable();
    const std::filesystem::path engineShaderPath = appDirectory / "ShaderPrecompiled/engine" / shaderTypeName;
    const std::filesystem::path appShaderPath = appDirectory / "ShaderPrecompiled/caustica" / shaderTypeName;
    const std::filesystem::path nrdShaderPath = appDirectory / "ShaderPrecompiled/nrd" / shaderTypeName;
    const std::filesystem::path ommShaderPath = appDirectory / "ShaderPrecompiled/omm" / shaderTypeName;

    std::shared_ptr<caustica::RootFileSystem> rootFS = std::make_shared<caustica::RootFileSystem>();
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
        rootFS->mount("/ShaderPrecompiled/nrd", nrdShaderPath);
        rootFS->mount("/ShaderPrecompiled/omm", ommShaderPath);
    }

    auto device = m_GpuDevice->GetDevice();
    m_ShaderFactory = std::make_shared<caustica::ShaderFactory>(device, rootFS, "/ShaderPrecompiled");
}

bool EditorApplication::IsSERSupported() const
{
    auto device = m_GpuDevice->GetDevice();

    const bool usingDX12 = device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;
    const bool deviceSupportsSER = device->queryFeatureSupport(nvrhi::Feature::ShaderExecutionReordering);
    const bool SERSupported = usingDX12 && deviceSupportsSER && !m_CmdLine.disableSER; // SER Only enabled in DX12 for now

    return SERSupported;
}

void EditorApplication::syncPassesToBackBuffer()
{
    if (!m_GpuDevice)
        return;

    const auto& params = m_GpuDevice->GetDeviceParams();
    notifyBackBufferResizing();
    notifyBackBufferResized(params.backBufferWidth, params.backBufferHeight, params.swapChainSampleCount);
}

void EditorApplication::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (m_scenePass && (windowFocused || m_scenePass->ShouldAnimateUnfocused()))
    {
        m_scenePass->Animate(elapsedTimeSeconds);
        m_scenePass->SetLatewarpOptions();
    }

    if (m_uiPass && (windowFocused || m_uiPass->ShouldAnimateUnfocused()))
    {
        caustica::ImGui_Renderer& ui = *m_uiPass;
        ui.Animate(elapsedTimeSeconds);
    }
}

void EditorApplication::onRender()
{
    caustica::GpuDevice* dm = getGpuDevice();
    if (!dm)
        return;

    if (m_scenePass)
        m_scenePass->Render(dm->GetCurrentFramebuffer(m_scenePass->SupportsDepthBuffer()));

    if (m_uiPass)
        m_uiPass->Render(dm->GetCurrentFramebuffer(m_uiPass->SupportsDepthBuffer()));
}

void EditorApplication::onBackBufferResizing()
{
    if (m_scenePass)
        m_scenePass->BackBufferResizing();
    if (m_uiPass)
        m_uiPass->BackBufferResizing();
}

void EditorApplication::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_scenePass)
        m_scenePass->BackBufferResized(width, height, sampleCount);
    if (m_uiPass)
        m_uiPass->BackBufferResized(width, height, sampleCount);
}

void EditorApplication::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_scenePass)
        m_scenePass->DisplayScaleChanged(scaleX, scaleY);
    if (m_uiPass)
    {
        caustica::ImGui_Renderer& ui = *m_uiPass;
        ui.DisplayScaleChanged(scaleX, scaleY);
    }
}

bool EditorApplication::shouldRenderWhenUnfocused() const
{
    return m_scenePass && m_scenePass->ShouldRenderUnfocused();
}

void EditorApplication::onEvent(caustica::Event& event)
{
    // 1. Dispatch input events to the input controller.
    if (m_scenePass)
        m_scenePass->GetInputController()->onEvent(event);

    // 2. Handle window events locally.
    caustica::EventDispatcher dispatcher(event);

    dispatcher.Dispatch<caustica::WindowCloseEvent>([this](caustica::WindowCloseEvent&) {
        if (m_Window)
            m_Window->setExit(true);
        return true;
    });
}
