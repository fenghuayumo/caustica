#pragma once

#include <rhi/rhi_types.h>
#include <dxgi.h>

namespace caustica::rhi
{
    struct DxgiFormatMapping
    {
        Format abstractFormat;
        DXGI_FORMAT resourceFormat;
        DXGI_FORMAT srvFormat;
        DXGI_FORMAT rtvFormat;
    };

    const DxgiFormatMapping& getDxgiFormatMapping(Format abstractFormat);
}