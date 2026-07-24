#pragma once

#include <rhi/rhi_types.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>

namespace caustica::rhi::ObjectTypes
{
    constexpr ObjectType CAUSTICA_RHI_D3D11_Device = 0x00010101;
};

namespace caustica::rhi::d3d11
{
    struct DeviceDesc
    {
        MessageCallback* messageCallback = nullptr;
        ID3D11DeviceContext* context = nullptr;
        bool aftermathEnabled = false;
    };

    CAUSTICA_RHI_API DeviceHandle createDevice(const DeviceDesc& desc);

    CAUSTICA_RHI_API DXGI_FORMAT convertFormat(caustica::rhi::Format format);
}

