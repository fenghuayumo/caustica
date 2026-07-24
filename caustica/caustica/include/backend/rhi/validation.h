#pragma once

#include <rhi/rhi_types.h>

namespace caustica::rhi::validation
{
    CAUSTICA_RHI_API DeviceHandle createValidationLayer(Device* underlyingDevice);
}

