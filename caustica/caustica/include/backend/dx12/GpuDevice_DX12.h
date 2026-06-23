#pragma once

#include <vector>

#include <backend/GpuDevice.h>

#include <Windows.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>

#include <rhi/d3d12.h>
#include <rhi/validation.h>

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

public:
    std::string GetAdapterName(DXGI_ADAPTER_DESC const& aDesc)
    {
        size_t length = wcsnlen(aDesc.Description, _countof(aDesc.Description));

        std::string name;
        name.resize(length);
        WideCharToMultiByte(CP_ACP, 0, aDesc.Description, int(length), name.data(), int(name.size()), nullptr, nullptr);

        return name;
    }

    const char *GetRendererString() const override
    {
        return m_RendererString.c_str();
    }

    nvrhi::IDevice *GetDevice() const override
    {
        return m_NvrhiDevice;
    }

    void ReportLiveObjects() override;
    bool EnumerateAdapters(std::vector<caustica::AdapterInfo>& outAdapters) override;

    nvrhi::GraphicsAPI GetGraphicsAPI() const override
    {
        return nvrhi::GraphicsAPI::D3D12;
    }
    
protected:
    bool CreateInstanceInternal() override;
    bool CreateDevice() override;
    bool CreateSwapChain() override;
    void DestroyDeviceAndSwapChain() override;
    void ResizeSwapChain() override;
    nvrhi::ITexture* GetCurrentBackBuffer() override;
    nvrhi::ITexture* GetBackBuffer(uint32_t index) override;
    uint32_t GetCurrentBackBufferIndex() override;
    uint32_t GetBackBufferCount() override;
    bool BeginFrame() override;
    bool Present() override;
    void Shutdown() override;
    bool CreateRenderTargets();
    void ReleaseRenderTargets();
};