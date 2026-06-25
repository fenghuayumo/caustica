#include "render/Core/RenderContext.h"
#include "backend/GpuDevice.h"

namespace caustica
{

nvrhi::IDevice* RenderContext::GetDevice() const
{
    return m_GpuDevice->GetDevice();
}

uint32_t RenderContext::GetFrameIndex() const
{
    return m_GpuDevice->GetFrameIndex();
}

} // namespace caustica
