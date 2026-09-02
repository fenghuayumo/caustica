#include "PythonDevice.h"

#if CAUSTICA_WITH_PYTHON

#include <engine/EntryPoint.h>
#include <backend/rhi/device_factory.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <rhi/rhi.h>

#include <array>
#include <mutex>
#include <stdexcept>
#include <string>

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
        return caustica::getDirectoryWithExecutable();
    }

    caustica::rhi::GraphicsAPI ResolveGraphicsApi(bool useVulkan)
    {
#if CAUSTICA_WITH_DX12 && CAUSTICA_WITH_VULKAN
        return useVulkan ? caustica::rhi::GraphicsAPI::VULKAN : caustica::rhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
        if (!useVulkan)
            caustica::warning("caustica.Device: DX12 was requested but this build only has Vulkan; using Vulkan.");
        return caustica::rhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX12
        if (useVulkan)
            caustica::warning("caustica.Device: Vulkan was requested but this build only has DX12; using DX12.");
        return caustica::rhi::GraphicsAPI::D3D12;
#else
        static_assert(CAUSTICA_WITH_DX12 || CAUSTICA_WITH_VULKAN, "Caustica requires at least one graphics backend");
#endif
    }

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    std::string GetAgilitySDKPath()
    {
        std::string sdkPath = (caustica_py::ResolveRuntimeDirectory() / "D3D12").string();
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
            caustica::warning("caustica.Device: D3D12 experimental shader models could not be enabled, HRESULT = 0x%08x", unsigned(hr));
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

            caustica::warning("caustica.Device: ID3D12SDKConfiguration1::CreateDeviceFactory('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
        }
        else
        {
            caustica::warning("caustica.Device: D3D12GetInterface(ID3D12SDKConfiguration1) failed, HRESULT = 0x%08x", unsigned(hr));
        }

        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
        hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));
        if (FAILED(hr))
        {
            caustica::warning("caustica.Device: D3D12GetInterface(ID3D12SDKConfiguration) failed, HRESULT = 0x%08x", unsigned(hr));
            return nullptr;
        }

        hr = sdkConfig->SetSDKVersion(CAUSTICA_D3D_AGILITY_SDK_VERSION, sdkPath.c_str());
        if (FAILED(hr))
        {
            caustica::warning("caustica.Device: ID3D12SDKConfiguration::SetSDKVersion('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
            return nullptr;
        }

        EnableD3D12ExperimentalShaderModels(nullptr);
        return nullptr;
    }
#endif
}

namespace caustica_py
{
    std::filesystem::path ResolveRuntimeDirectory()
    {
        const std::filesystem::path moduleDirectory = GetCurrentModuleDirectory();
        if (std::filesystem::exists(moduleDirectory / "ShaderBin"))
            return moduleDirectory;

        const std::filesystem::path executableDirectory = caustica::getDirectoryWithExecutable();
        if (std::filesystem::exists(executableDirectory / "ShaderBin"))
            return executableDirectory;

        return moduleDirectory;
    }

    PythonDevice::PythonDevice(const Config& cfg)
        : m_config(cfg)
    {
        AcquireAppPlatform();
        m_platformInitialized = true;

        if (cfg.nonInteractive)
        {
            caustica::enableOutputToMessageBox(false);
            caustica::enableOutputToConsole(true);
            caustica::setMinSeverity(caustica::Severity::Warning);
            caustica::helpersSetNonInteractive();
        }

        std::string adapterError;
        if (!caustica::rhi::parseAdapterSelector(cfg.adapter, m_adapter, &adapterError))
        {
            ReleaseAppPlatform();
            m_platformInitialized = false;
            throw std::runtime_error("caustica.Device: invalid GPU adapter selector '" + cfg.adapter + "': " + adapterError);
        }

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        if (!cfg.useVulkan)
            m_d3d12DeviceFactory = CreateD3D12AgilityDeviceFactory();
#endif
    }

    PythonDevice::~PythonDevice()
    {
        close();
    }

    bool PythonDevice::ensureCreated(int width, int height, bool headless)
    {
        if (width <= 0 || height <= 0)
        {
            caustica::error("caustica.Device: back-buffer size must be positive");
            return false;
        }

        if (m_gpu)
        {
            if (m_width != width || m_height != height || m_headless != headless)
            {
                caustica::error(
                    "caustica.Device: GPU already created at %dx%d headless=%d; cannot rebind as %dx%d headless=%d",
                    m_width, m_height, int(m_headless), width, height, int(headless));
                return false;
            }
            return true;
        }

        caustica::GpuDeviceCreateDesc createDesc{};
        createDesc.api = ResolveGraphicsApi(m_config.useVulkan);
        createDesc.headless = headless;
        createDesc.windowTitle = "caustica_py";
        createDesc.backBufferWidth = uint32_t(width);
        createDesc.backBufferHeight = uint32_t(height);
        createDesc.adapter = m_adapter;
        createDesc.enableDebug = m_config.debug;
        if (headless)
            createDesc.vsyncEnabled = false;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        createDesc.d3d12DeviceFactory = m_d3d12DeviceFactory.Get();
#endif

        caustica::GpuDeviceCreateResult result = caustica::GpuDevice::create(createDesc);
        if (!result.gpuDevice || !result.surface || (!headless && !result.window))
        {
            caustica::error("caustica.Device: failed to create GPU device / surface");
            return false;
        }

        m_gpu = std::move(result.gpuDevice);
        m_window = std::move(result.window);
        m_surface = std::move(result.surface);
        m_width = width;
        m_height = height;
        m_headless = headless;
        return true;
    }

    bool PythonDevice::tryBind()
    {
        if (m_bound)
        {
            caustica::error("caustica.Device: already bound to an App");
            return false;
        }
        if (!m_gpu)
        {
            caustica::error("caustica.Device: GPU is not created yet");
            return false;
        }
        m_bound = true;
        return true;
    }

    void PythonDevice::unbind()
    {
        m_bound = false;
    }

    void PythonDevice::shutdownGpu()
    {
        m_surface.reset();
        if (m_gpu)
        {
            m_gpu->shutdown();
            m_gpu.reset();
        }
        m_window.reset();
        m_width = 0;
        m_height = 0;
    }

    void PythonDevice::close()
    {
        if (m_bound)
        {
            caustica::error("caustica.Device: close() called while an App still holds the device; close the App first");
            return;
        }

        shutdownGpu();

        if (m_platformInitialized)
        {
            ReleaseAppPlatform();
            m_platformInitialized = false;
        }
    }

    std::optional<caustica::AdapterInfo> PythonDevice::selectedAdapter() const
    {
        if (!m_gpu || !m_gpu->getSelectedAdapter())
            return std::nullopt;
        return *m_gpu->getSelectedAdapter();
    }
}

#endif // CAUSTICA_WITH_PYTHON
