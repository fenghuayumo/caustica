#pragma once

#include <render/core/BuiltinTextures.h>
#include <render/core/FullscreenBlitPass.h>
#include <render/core/StandardSamplers.h>
#include <rhi/nvrhi.h>

#include <memory>

namespace caustica
{
class ShaderFactory;
}

namespace caustica::render
{

struct SceneGpuResources;

class RenderDevice
{
public:
    RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~RenderDevice();

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    [[nodiscard]] nvrhi::IDevice* getDevice() const { return m_device; }

    [[nodiscard]] BuiltinTextures& builtins() { return *m_builtins; }
    [[nodiscard]] const BuiltinTextures& builtins() const { return *m_builtins; }

    [[nodiscard]] StandardSamplers& samplers() { return *m_samplers; }
    [[nodiscard]] const StandardSamplers& samplers() const { return *m_samplers; }

    [[nodiscard]] FullscreenBlitPass& blit() { return *m_blit; }
    [[nodiscard]] const FullscreenBlitPass& blit() const { return *m_blit; }
    void setActiveSceneGpuResources(SceneGpuResources* resources) { m_activeSceneGpuResources = resources; }
    [[nodiscard]] SceneGpuResources* activeSceneGpuResources() const { return m_activeSceneGpuResources; }

private:
    nvrhi::IDevice* m_device = nullptr;
    std::unique_ptr<BuiltinTextures> m_builtins;
    std::unique_ptr<StandardSamplers> m_samplers;
    std::unique_ptr<FullscreenBlitPass> m_blit;
    SceneGpuResources* m_activeSceneGpuResources = nullptr;
};

} // namespace caustica::render
