#include <backend/renderContext.h>
#include <backend/GpuDevice.h>

namespace caustica
{

caustica::rhi::Device* renderContext::getDevice() const
{
    return m_GpuDevice->getDevice();
}

uint32_t renderContext::getFrameIndex() const
{
    return m_GpuDevice->getFrameIndex();
}

} // namespace caustica
