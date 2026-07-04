#pragma once

#include <rhi/Device.h>

#include <memory>

namespace caustica
{
class CommonRenderPasses;
class ShaderFactory;
} // namespace caustica

namespace caustica::rhi
{

class BuiltinTextures;
class StandardSamplers;
class FullscreenBlitPass;

// R0 entry point: engine-owned GPU utilities (successor to CommonRenderPasses).
// Phase B1: facade shell delegating to CommonRenderPasses until sub-types are split out.
class RenderDevice
{
public:
    RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~RenderDevice();

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    [[nodiscard]] Device& device() { return m_device; }
    [[nodiscard]] const Device& device() const { return m_device; }

    // Legacy bridge — removed once call sites migrate off CommonRenderPasses.
    [[nodiscard]] caustica::CommonRenderPasses& commonPasses();
    [[nodiscard]] const caustica::CommonRenderPasses& commonPasses() const;
    [[nodiscard]] std::shared_ptr<caustica::CommonRenderPasses> commonPassesPtr() const { return m_commonPasses; }

    // Future R0 splits (return nullptr until implemented):
    [[nodiscard]] BuiltinTextures* builtins() { return nullptr; }
    [[nodiscard]] StandardSamplers* samplers() { return nullptr; }
    [[nodiscard]] FullscreenBlitPass* blit() { return nullptr; }

private:
    Device m_device;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
};

} // namespace caustica::rhi
