#pragma once

#include <cstdint>

namespace caustica
{

// Callback surface for GpuDevice swap-chain / back-buffer lifecycle events.
// Implemented by engine::Application (or any other frame owner); keeps backend
// independent of the engine layer.
class IGpuFrameDriver
{
public:
    virtual ~IGpuFrameDriver() = default;

    virtual void notifyBackBufferResizing() = 0;
    virtual void notifyBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) = 0;
};

} // namespace caustica
