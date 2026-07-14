#pragma once

#include <vector>

#include <backend/GpuDevice.h>

#include <Windows.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>

#include <rhi/d3d12.h>
#include <rhi/validation.h>

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <wrl/client.h>
#endif

class GpuDevice_DX12 : public caustica::GpuDevice
{
protected:
    nvrhi::RefCountPtr<IDXGIFactory2>               m_DxgiFactory2;
    nvrhi::RefCountPtr<ID3D12Device>                m_Device12;
    nvrhi::RefCountPtr<ID3D12CommandQueue>          m_GraphicsQueue;
    nvrhi::RefCountPtr<ID3D12CommandQueue>          m_ComputeQueue;
    nvrhi::RefCountPtr<ID3D12CommandQueue>          m_CopyQueue;
    nvrhi::RefCountPtr<IDXGISwapChain3>             m_SwapChain;
    DXGI_SWAP_CHAIN_DESC1                           m_SwapChainDesc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC                 m_FullScreenDesc{};
    nvrhi::RefCountPtr<IDXGIAdapter>                m_DxgiAdapter;
    HWND                                            m_hWnd = nullptr;
    bool                                            m_TearingSupported = false;

    std::vector<nvrhi::RefCountPtr<ID3D12Resource>> m_SwapChainBuffers;
    std::vector<nvrhi::TextureHandle>               m_RhiSwapChainBuffers;
    nvrhi::RefCountPtr<ID3D12Fence>                 m_FrameFence;
    std::vector<HANDLE>                             m_FrameFenceEvents;

    UINT64                                          m_FrameCount = 1;

    nvrhi::DeviceHandle                             m_NvrhiDevice;

    std::string                                     m_RendererString;

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory>     m_OwnedD3d12DeviceFactory;
#endif

public:
    std::string getAdapterName(DXGI_ADAPTER_DESC const& aDesc)
    {
        size_t length = wcsnlen(aDesc.Description, _countof(aDesc.Description));

        std::string name;
        name.resize(length);
        WideCharToMultiByte(CP_ACP, 0, aDesc.Description, int(length), name.data(), int(name.size()), nullptr, nullptr);

        return name;
    }

    const char *getRendererString() const override
    {
        return m_RendererString.c_str();
    }

    nvrhi::IDevice *getDevice() const override
    {
        return m_NvrhiDevice;
    }

    void reportLiveObjects() override;
    bool enumerateAdapters(std::vector<caustica::AdapterInfo>& outAdapters) override;
    [[nodiscard]] bool queryVideoMemoryInfo(caustica::VideoMemoryInfo& out) const override;

    nvrhi::GraphicsAPI getGraphicsAPI() const override
    {
        return nvrhi::GraphicsAPI::D3D12;
    }
    
protected:
    bool createInstanceInternal() override;
    bool createDevice() override;
    bool createSwapChain() override;
    void destroyDeviceAndSwapChain() override;
    void resizeSwapChain() override;
    nvrhi::ITexture* getCurrentBackBuffer() override;
    nvrhi::ITexture* getBackBuffer(uint32_t index) override;
    uint32_t getCurrentBackBufferIndex() override;
    uint32_t getBackBufferCount() override;
    bool beginFrame() override;
    bool present() override;
    void shutdown() override;
    void prepareShutdown() override;
    bool createRenderTargets();
    void releaseRenderTargets();
};