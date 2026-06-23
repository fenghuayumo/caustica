#pragma once

#include <rhi/nvrhi.h>

namespace nvrhi::validation
{
    NVRHI_API DeviceHandle createValidationLayer(IDevice* underlyingDevice);
}
