#pragma once

#if CAUSTICA_WITH_PYTHON

#include <backend/GpuDevice.h>
#include <backend/GpuSurface.h>
#include <platform/window.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace caustica_py
{
    std::filesystem::path ResolveRuntimeDirectory();

    // Owns the logical GPU plus its presentation surface (window swapchain or
    // headless targets). Creation is lazy: adapter/API are fixed at construction;
    // size / headless are applied on the first App bind.
    class PythonDevice
    {
    public:
        struct Config
        {
#if CAUSTICA_WITH_DX12
            bool useVulkan = false;
#else
            bool useVulkan = true;
#endif
            std::string adapter = "auto";
            bool debug = false;
            bool nonInteractive = true;
        };

        explicit PythonDevice(const Config& cfg);
        ~PythonDevice();

        PythonDevice(const PythonDevice&) = delete;
        PythonDevice& operator=(const PythonDevice&) = delete;

        // Create the GPU, surface, and optional window if needed. A second call
        // must request the same width/height/headless or it fails.
        bool ensureCreated(int width, int height, bool headless);
        [[nodiscard]] bool isCreated() const { return m_gpu != nullptr; }

        // One live App per Device. A second bind fails until unbind().
        bool tryBind();
        void unbind();
        [[nodiscard]] bool isBound() const { return m_bound; }

        void close();

        [[nodiscard]] caustica::GpuDevice* gpu() { return m_gpu.get(); }
        [[nodiscard]] const caustica::GpuDevice* gpu() const { return m_gpu.get(); }
        [[nodiscard]] caustica::Window* window() { return m_window.get(); }
        [[nodiscard]] caustica::GpuSurface* surface() { return m_surface.get(); }
        [[nodiscard]] const Config& config() const { return m_config; }
        [[nodiscard]] const caustica::rhi::AdapterSelector& adapterSelector() const { return m_adapter; }
        [[nodiscard]] bool useVulkan() const { return m_config.useVulkan; }
        [[nodiscard]] int width() const { return m_width; }
        [[nodiscard]] int height() const { return m_height; }
        [[nodiscard]] bool headless() const { return m_headless; }
        [[nodiscard]] std::optional<caustica::AdapterInfo> selectedAdapter() const;

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        [[nodiscard]] ID3D12DeviceFactory* d3d12Factory() const { return m_d3d12DeviceFactory.Get(); }
#endif

    private:
        void shutdownGpu();

        Config m_config{};
        caustica::rhi::AdapterSelector m_adapter{};
        int m_width = 0;
        int m_height = 0;
        bool m_headless = true;
        bool m_bound = false;
        bool m_platformInitialized = false;

        std::unique_ptr<caustica::GpuDevice> m_gpu;
        std::unique_ptr<caustica::Window> m_window;
        std::unique_ptr<caustica::GpuSurface> m_surface;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        Microsoft::WRL::ComPtr<ID3D12DeviceFactory> m_d3d12DeviceFactory;
#endif
    };
}

#endif // CAUSTICA_WITH_PYTHON
