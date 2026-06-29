#include "EditorSessionHost.h"

namespace caustica::editor
{

void syncPathTracerSessionBackBuffer(caustica::GpuDevice& gpuDevice, caustica::Application& frameDriver)
{
    if (!gpuDevice.GetDevice())
        return;

    const caustica::BackBufferInfo backBuffer = gpuDevice.GetBackBufferInfo();
    frameDriver.notifyBackBufferResizing();
    frameDriver.notifyBackBufferResized(backBuffer.width, backBuffer.height, backBuffer.sampleCount);
}

} // namespace caustica::editor
