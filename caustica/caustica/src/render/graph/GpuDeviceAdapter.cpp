#include <render/graph/GpuDeviceAdapter.h>

#include <rhi/nvrhi.h>

namespace caustica::rg
{

Device::Device(nvrhi::IDevice* device)
    : m_device(device)
{
}

TextureHandle Device::importTexture(nvrhi::ITexture* texture)
{
    TextureHandle handle{};
    handle.index = texture ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture) & 0xFFFFFFFFu) : UINT32_MAX;
    return handle;
}

nvrhi::ITexture* Device::resolveTexture(TextureHandle handle) const
{
    if (!handle.isValid())
        return nullptr;
    return reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(handle.index));
}

BufferHandle Device::importBuffer(nvrhi::IBuffer* buffer)
{
    BufferHandle handle{};
    handle.index = buffer ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer) & 0xFFFFFFFFu) : UINT32_MAX;
    return handle;
}

nvrhi::IBuffer* Device::resolveBuffer(BufferHandle handle) const
{
    if (!handle.isValid())
        return nullptr;
    return reinterpret_cast<nvrhi::IBuffer*>(static_cast<uintptr_t>(handle.index));
}

bool Device::waitForIdle() const
{
    return m_device && m_device->waitForIdle();
}

CommandList::CommandList(nvrhi::ICommandList* commandList)
    : m_commandList(commandList)
{
}

} // namespace caustica::rg
