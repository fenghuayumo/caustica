#include <backend/RenderContext.h>
#include <backend/GpuDevice.h>

namespace caustica
{

nvrhi::IDevice* RenderContext::getDevice() const
{
    return m_GpuDevice->getDevice();
}

uint32_t RenderContext::GetFrameIndex() const
{
    return m_GpuDevice->GetFrameIndex();
}

} // namespace caustica
