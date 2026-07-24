#pragma once

#include <rhi/rhi_types.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <directx/d3d12.h>

namespace caustica::rhi
{
    namespace ObjectTypes
    {
        constexpr ObjectType CAUSTICA_RHI_D3D12_Device         = 0x00020101;
        constexpr ObjectType CAUSTICA_RHI_D3D12_CommandList    = 0x00020102;
    };
}

namespace caustica::rhi::d3d12
{
    class RootSignature;
    typedef RefCountPtr<RootSignature> RootSignatureHandle;

    // D3D12-specific CommandList methods live on the concrete backend type
    // (caustica::rhi::d3d12::CommandList). Use checked_cast when needed.
    typedef caustica::rhi::CommandListHandle CommandListHandle;

    typedef uint32_t DescriptorIndex;

    class DescriptorHeap
    {
    protected:
        DescriptorHeap() = default;
        virtual ~DescriptorHeap() = default;
    public:
        virtual DescriptorIndex allocateDescriptors(uint32_t count) = 0;
        virtual DescriptorIndex allocateDescriptor() = 0;
        virtual void releaseDescriptors(DescriptorIndex baseIndex, uint32_t count) = 0;
        virtual void releaseDescriptor(DescriptorIndex index) = 0;
        virtual D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle(DescriptorIndex index) = 0;
        virtual D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandleShaderVisible(DescriptorIndex index) = 0;
        virtual D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle(DescriptorIndex index) = 0;
        [[nodiscard]] virtual ID3D12DescriptorHeap* getHeap() const = 0;
        [[nodiscard]] virtual ID3D12DescriptorHeap* getShaderVisibleHeap() const = 0;

        DescriptorHeap(const DescriptorHeap&) = delete;
        DescriptorHeap(const DescriptorHeap&&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&&) = delete;
    };

    enum class DescriptorHeapType
    {
        RenderTargetView,
        DepthStencilView,
        ShaderResourceView,
        Sampler
    };

    // D3D12-specific Device methods live on the concrete backend type
    // (caustica::rhi::d3d12::Device). Use checked_cast when needed.
    typedef caustica::rhi::DeviceHandle DeviceHandle;

    struct DeviceDesc
    {
        MessageCallback* errorCB = nullptr;
        ID3D12Device* pDevice = nullptr;
        ID3D12CommandQueue* pGraphicsCommandQueue = nullptr;
        ID3D12CommandQueue* pComputeCommandQueue = nullptr;
        ID3D12CommandQueue* pCopyCommandQueue = nullptr;

        uint32_t renderTargetViewHeapSize = 1024;
        uint32_t depthStencilViewHeapSize = 1024;
        uint32_t shaderResourceViewHeapSize = 16384;
        uint32_t samplerHeapSize = 1024;
        uint32_t maxTimerQueries = 256;

        // If enabled and the device has the capability,
        // create RootSignatures with D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED 
        // and D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
        bool enableHeapDirectlyIndexed = false;

        bool aftermathEnabled = false;

        // Enable logging the buffer lifetime to MessageCallback
        // Useful for debugging resource lifetimes
        bool logBufferLifetime = false;
    };

    CAUSTICA_RHI_API DeviceHandle createDevice(const DeviceDesc& desc);

    CAUSTICA_RHI_API DXGI_FORMAT convertFormat(caustica::rhi::Format format);
}

