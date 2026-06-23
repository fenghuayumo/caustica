#pragma once

#include <rhi/nvrhi.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>

namespace nvrhi::ObjectTypes
{
    constexpr ObjectType Nvrhi_D3D11_Device = 0x00010101;
};

namespace nvrhi::d3d11
{
    struct DeviceDesc
    {
        IMessageCallback* messageCallback = nullptr;
        ID3D11DeviceContext* context = nullptr;
        bool aftermathEnabled = false;
    };

    NVRHI_API DeviceHandle createDevice(const DeviceDesc& desc);

    NVRHI_API DXGI_FORMAT convertFormat(nvrhi::Format format);
}
