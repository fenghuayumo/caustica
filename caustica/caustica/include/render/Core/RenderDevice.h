#pragma once

#include <render/Core/BuiltinTextures.h>
#include <render/Core/FullscreenBlitPass.h>
#include <render/Core/StandardSamplers.h>
#include <render/graph/GpuDeviceAdapter.h>

#include <memory>

namespace caustica
{
class ShaderFactory;
}

namespace caustica::render
{

class RenderDevice
{
public:
    RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~RenderDevice();

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    [[nodiscard]] rg::Device& device() { return m_device; }
    [[nodiscard]] const rg::Device& device() const { return m_device; }

    [[nodiscard]] BuiltinTextures& builtins() { return *m_builtins; }
    [[nodiscard]] const BuiltinTextures& builtins() const { return *m_builtins; }

    [[nodiscard]] StandardSamplers& samplers() { return *m_samplers; }
    [[nodiscard]] const StandardSamplers& samplers() const { return *m_samplers; }

    [[nodiscard]] FullscreenBlitPass& blit() { return *m_blit; }
    [[nodiscard]] const FullscreenBlitPass& blit() const { return *m_blit; }

private:
    rg::Device m_device;
    std::unique_ptr<BuiltinTextures> m_builtins;
    std::unique_ptr<StandardSamplers> m_samplers;
    std::unique_ptr<FullscreenBlitPass> m_blit;
};

} // namespace caustica::render
