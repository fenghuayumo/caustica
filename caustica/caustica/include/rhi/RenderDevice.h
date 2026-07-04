#pragma once

#include <rhi/BuiltinTextures.h>
#include <rhi/Device.h>
#include <rhi/FullscreenBlitPass.h>
#include <rhi/StandardSamplers.h>

#include <memory>

namespace caustica
{
class CommonRenderPasses;
class ShaderFactory;
} // namespace caustica

namespace caustica::rhi
{

class RenderDevice
{
public:
    RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~RenderDevice();

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    [[nodiscard]] Device& device() { return m_device; }
    [[nodiscard]] const Device& device() const { return m_device; }

    [[nodiscard]] BuiltinTextures& builtins() { return *m_builtins; }
    [[nodiscard]] const BuiltinTextures& builtins() const { return *m_builtins; }

    [[nodiscard]] StandardSamplers& samplers() { return *m_samplers; }
    [[nodiscard]] const StandardSamplers& samplers() const { return *m_samplers; }

    [[nodiscard]] FullscreenBlitPass& blit() { return *m_blit; }
    [[nodiscard]] const FullscreenBlitPass& blit() const { return *m_blit; }

    // Legacy bridge — removed once call sites migrate off CommonRenderPasses.
    [[nodiscard]] caustica::CommonRenderPasses& commonPasses();
    [[nodiscard]] const caustica::CommonRenderPasses& commonPasses() const;
    [[nodiscard]] std::shared_ptr<caustica::CommonRenderPasses> commonPassesPtr() const { return m_commonPasses; }

private:
    Device m_device;
    std::unique_ptr<BuiltinTextures> m_builtins;
    std::unique_ptr<StandardSamplers> m_samplers;
    std::unique_ptr<FullscreenBlitPass> m_blit;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
};

} // namespace caustica::rhi
