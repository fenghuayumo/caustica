#include "render/Core/IRenderPass.h"
#include "backend/GpuDevice.h"

namespace caustica
{

nvrhi::IDevice* IRenderPass::GetDevice() const
{
    return m_GpuDevice->GetDevice();
}

uint32_t IRenderPass::GetFrameIndex() const
{
    return m_GpuDevice->GetFrameIndex();
}

} // namespace caustica
