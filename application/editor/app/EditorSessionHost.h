#pragma once

#include <backend/GpuFrameDriver.h>
#include <engine/Application.h>

namespace caustica
{
class GpuDevice;
}

namespace caustica::editor
{

// Notify scene passes and UI that the swap chain matches the current back buffer.
void syncPathTracerSessionBackBuffer(caustica::GpuDevice& gpuDevice, caustica::Application& frameDriver);

} // namespace caustica::editor
