#pragma once

#include <render/BindingCache.h>
#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <vector>

namespace caustica
{
    class ShaderFactory;
    class CommonRenderPasses;
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
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            nvrhi::TextureHandle texture,
            Mode mode = Mode::MODE_MAX);

        // Dispatches reduction kernel : reads LOD 0 and populates
        // LOD 1 and up
        void Dispatch(nvrhi::ICommandList* commandList, int maxLOD=-1);

        // debug : blits mip-map levels in spiral pattern to 'target'
        // (assumes 'target' texture resolution is high enough...)
        void Display(
            std::shared_ptr<caustica::CommonRenderPasses> commonPasses,
            nvrhi::ICommandList* commandList, 
            nvrhi::IFramebuffer* target);

    private:

        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_Shader;
        nvrhi::TextureHandle m_Texture;
        nvrhi::BufferHandle m_ConstantBuffer;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        std::vector<nvrhi::BindingSetHandle> m_BindingSets;
        nvrhi::ComputePipelineHandle m_Pso;

        // Set of unique dummy textures - see details in class implementation
        struct NullTextures;
        std::shared_ptr<NullTextures> m_NullTextures;

        caustica::BindingCache m_BindingCache;

    };

} // end namespace caustica::render
