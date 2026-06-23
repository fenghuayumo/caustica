#include "backend/GpuDevice.h"
#include "engine/DeviceManager.h"  // DeviceCreationParameters
#include <core/log.h>

#ifdef _WINDOWS
#include <windows.h>
#endif

namespace caustica {

bool GpuDevice::createInstance(const DeviceCreationParameters& params)
{
    if (instanceCreated) return true;
    m_Params = &params;

    if (!params.headlessDevice)
    {
        if (!glfwInit())
            return false;
    }
    // createInstanceInternal() is called by the subclass (DeviceManager)
    // which sets instanceCreated = true on success
    return true;
}

nvrhi::IFramebuffer* GpuDevice::getCurrentFramebuffer(bool withDepth)
{
    return getFramebuffer(currentBackBufferIndex, withDepth);
}

nvrhi::IFramebuffer* GpuDevice::getFramebuffer(uint32_t index, bool withDepth)
{
    if (withDepth)
        return index < framebuffersWithDepth.size() ? framebuffersWithDepth[index].Get() : nullptr;
    else
        return index < framebuffers.size() ? framebuffers[index].Get() : nullptr;
}

} // namespace caustica
