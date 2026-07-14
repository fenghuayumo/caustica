#pragma once

#include <string>

#include <backend/GpuDevice.h>
#include <core/log.h>

#include <Windows.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>

#include <rhi/d3d11.h>
#include <rhi/validation.h>

class GpuDevice_DX11 : public caustica::GpuDevice
{
protected:
    nvrhi::RefCountPtr<IDXGIFactory1> m_DxgiFactory;
    nvrhi::RefCountPtr<IDXGIAdapter> m_DxgiAdapter;
    nvrhi::RefCountPtr<ID3D11Device> m_Device;
    nvrhi::RefCountPtr<ID3D11DeviceContext> m_ImmediateContext;
    nvrhi::RefCountPtr<IDXGISwapChain> m_SwapChain;
    DXGI_SWAP_CHAIN_DESC m_SwapChainDesc{};
    HWND m_hWnd = nullptr;

    nvrhi::DeviceHandle m_NvrhiDevice;
    nvrhi::TextureHandle m_RhiBackBuffer;
    nvrhi::RefCountPtr<ID3D11Texture2D> m_D3D11BackBuffer;

    std::string m_RendererString;

public:
    [[nodiscard]] std::string getAdapterName(DXGI_ADAPTER_DESC const& aDesc)
    {
        size_t length = wcsnlen(aDesc.Description, _countof(aDesc.Description));

        std::string name;
        name.resize(length);
        WideCharToMultiByte(CP_ACP, 0, aDesc.Description, int(length), name.data(), int(name.size()), nullptr, nullptr);

        return name;
    }

    [[nodiscard]] const char* getRendererString() const override
    {
        return m_RendererString.c_str();
    }

    [[nodiscard]] nvrhi::IDevice* getDevice() const override
    {
        return m_NvrhiDevice;
    }

    bool beginFrame() override;
    void reportLiveObjects() override;
    bool enumerateAdapters(std::vector<caustica::AdapterInfo>& outAdapters) override;
    [[nodiscard]] bool queryVideoMemoryInfo(caustica::VideoMemoryInfo& out) const override;

    [[nodiscard]] nvrhi::GraphicsAPI getGraphicsAPI() const override
    {
        return nvrhi::GraphicsAPI::D3D11;
    }
protected:
    bool createInstanceInternal() override;
    bool createDevice() override;
    bool createSwapChain() override;
    void destroyDeviceAndSwapChain() override;
    void resizeSwapChain() override;
    void shutdown() override;

    nvrhi::ITexture* getCurrentBackBuffer() override
    {
        return m_RhiBackBuffer;
    }

    nvrhi::ITexture* getBackBuffer(uint32_t index) override
    {
        if (index == 0)
            return m_RhiBackBuffer;

        return nullptr;
    }

    uint32_t getCurrentBackBufferIndex() override
    {
        return 0;
    }

    uint32_t getBackBufferCount() override
    {
        return 1;
    }

    bool present() override;
    bool createRenderTarget();
    void releaseRenderTarget();
};