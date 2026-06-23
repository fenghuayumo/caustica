#pragma once

namespace nvrhi {
    class IDevice;
}

namespace caustica {

// =============================================================================
// GpuDevice — Backend layer: owns the NVRHI device pointer.
//
// Extracted from DeviceManager. DeviceManager still handles platform-specific
// creation (CreateInstanceInternal/CreateDevice), but stores the result here.
// =============================================================================
struct GpuDevice
{
    nvrhi::IDevice*  device = nullptr;
    bool             instanceCreated = false;

    bool isCreated() const { return device != nullptr; }
};

} // namespace caustica
