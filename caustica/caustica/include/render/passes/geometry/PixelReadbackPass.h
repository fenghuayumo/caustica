#pragma once

#include <math/math.h>
#include <memory>
#include <map>
#include <rhi/rhi.h>


namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
}

namespace caustica::render
{
    class PixelReadbackPass
    {
    private:
        caustica::rhi::DeviceHandle m_device;
        caustica::rhi::ShaderHandle m_shader;
        caustica::rhi::ComputePipelineHandle m_pipeline;
        caustica::rhi::BindingLayoutHandle m_bindingLayout;
        caustica::rhi::BindingSetHandle m_bindingSet;
        caustica::rhi::BufferHandle m_constantBuffer;
        caustica::rhi::BufferHandle m_intermediateBuffer;
        caustica::rhi::BufferHandle m_readbackBuffer;

    public:
        PixelReadbackPass(
            caustica::rhi::Device* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::rhi::Texture* inputTexture,
            caustica::rhi::Format format,
            uint32_t arraySlice = 0,
            uint32_t mipLevel = 0);

        void capture(caustica::rhi::CommandList* commandList, math::uint2 pixelPosition);

        math::float4 readFloats();
        math::uint4 readUInts();
        math::int4 readInts();
    };
}