#pragma once

#include <rhi/nvrhi.h>
#include <dxgi.h>

namespace nvrhi
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