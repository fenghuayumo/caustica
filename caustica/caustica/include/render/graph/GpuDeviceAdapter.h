#pragma once

#include <render/graph/GpuTypes.h>

namespace nvrhi
{
class IDevice;
class ICommandList;
class ITexture;
class IBuffer;
} // namespace nvrhi

namespace caustica::rg
{

class Device
{
public:
    explicit Device(nvrhi::IDevice* device);

    [[nodiscard]] nvrhi::IDevice* nativeDevice() const { return m_device; }

    [[nodiscard]] TextureHandle importTexture(nvrhi::ITexture* texture);
    [[nodiscard]] nvrhi::ITexture* resolveTexture(TextureHandle handle) const;

    [[nodiscard]] BufferHandle importBuffer(nvrhi::IBuffer* buffer);
    [[nodiscard]] nvrhi::IBuffer* resolveBuffer(BufferHandle handle) const;

    [[nodiscard]] bool waitForIdle() const;

private:
    nvrhi::IDevice* m_device = nullptr;
};

class CommandList
{
public:
    explicit CommandList(nvrhi::ICommandList* commandList);

    [[nodiscard]] nvrhi::ICommandList* nativeCommandList() const { return m_commandList; }

private:
    nvrhi::ICommandList* m_commandList = nullptr;
};

} // namespace caustica::rg
