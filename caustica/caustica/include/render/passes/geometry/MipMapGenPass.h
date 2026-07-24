#pragma once

#include <render/core/BindingCache.h>
#include <math/math.h>
#include <rhi/rhi.h>
#include <memory>
#include <vector>

namespace caustica
{
    class ShaderFactory;
    namespace render { class RenderDevice; }
}

// Compute reduction pass to generate mipmap levels

namespace caustica::render
{

    class MipMapGenPass {

    public:

        enum Mode : uint8_t {
            MODE_COLOR = 0,  // bilinear reduction of RGB channels
            MODE_MIN = 1,    // min() reduction of R channel
            MODE_MAX = 2,    // max() reduction of R channel
            MODE_MINMAX = 3, // min() and max() reductions of R channel into RG channels
        };

        // note : 'texture' must have been allocated with some mip levels
        MipMapGenPass(
            caustica::rhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::rhi::TextureHandle texture,
            Mode mode = Mode::MODE_MAX);

        // Dispatches reduction kernel : reads LOD 0 and populates
        // LOD 1 and up
        void dispatch(caustica::rhi::ICommandList* commandList, int maxLOD=-1);

        // debug : blits mip-map levels in spiral pattern to 'target'
        // (assumes 'target' texture resolution is high enough...)
        void display(
            caustica::render::RenderDevice& renderDevice,
            caustica::rhi::ICommandList* commandList, 
            caustica::rhi::IFramebuffer* target);

    private:

        caustica::rhi::DeviceHandle m_device;
        caustica::rhi::ShaderHandle m_Shader;
        caustica::rhi::TextureHandle m_Texture;
        caustica::rhi::BufferHandle m_ConstantBuffer;
        caustica::rhi::BindingLayoutHandle m_BindingLayout;
        std::vector<caustica::rhi::BindingSetHandle> m_BindingSets;
        caustica::rhi::ComputePipelineHandle m_Pso;

        // Set of unique dummy textures - see details in class implementation
        struct NullTextures;
        std::shared_ptr<NullTextures> m_NullTextures;

        caustica::BindingCache m_BindingCache;

    };

} // end namespace caustica::render
