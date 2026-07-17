#pragma once

#include <memory>

#include <rhi/nvrhi.h>

namespace caustica
{

namespace render
{
class RenderDevice;
}

class AssetSystem;
class BindingCache;
class BindlessTable;
class DescriptorTableManager;
class GpuDevice;
class ShaderFactory;
class TextureLoader;

// GPU device-side caches shared by path tracing and scene loaders.
// Owned as an App resource; PathTracingContext holds references into these members.
struct RenderInfra
{
    RenderInfra();
    ~RenderInfra();

    RenderInfra(const RenderInfra&) = delete;
    RenderInfra& operator=(const RenderInfra&) = delete;
    RenderInfra(RenderInfra&&) noexcept;
    RenderInfra& operator=(RenderInfra&&) noexcept;

    nvrhi::BindingLayoutHandle bindlessLayout;
    std::shared_ptr<ShaderFactory> shaderFactory;
    std::unique_ptr<render::RenderDevice> renderDevice;
    std::unique_ptr<BindingCache> bindingCache;
    std::unique_ptr<BindlessTable> bindlessTable;
    std::shared_ptr<DescriptorTableManager> descriptorTable;
    std::shared_ptr<TextureLoader> textureLoader;

    bool initialize(GpuDevice& gpuDevice, AssetSystem& assetSystem);
    void endFrame();
    void shutdown();

    [[nodiscard]] render::RenderDevice& device();
    [[nodiscard]] const render::RenderDevice& device() const;
};

} // namespace caustica
