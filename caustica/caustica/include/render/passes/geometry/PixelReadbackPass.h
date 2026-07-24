#pragma once

#include <math/math.h>
#include <memory>
#include <map>
#include <rhi/rhi.h>


namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
    class PixelReadbackPass
    {
    private:
        caustica::rhi::DeviceHandle m_device;
        caustica::rhi::ShaderHandle m_Shader;
        caustica::rhi::ComputePipelineHandle m_Pipeline;
        caustica::rhi::BindingLayoutHandle m_BindingLayout;
        caustica::rhi::BindingSetHandle m_BindingSet;
        caustica::rhi::BufferHandle m_ConstantBuffer;
        caustica::rhi::BufferHandle m_IntermediateBuffer;
        caustica::rhi::BufferHandle m_ReadbackBuffer;

    public:
        PixelReadbackPass(
            caustica::rhi::Device* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::rhi::Texture* inputTexture,
            caustica::rhi::Format format,
            uint32_t arraySlice = 0,
            uint32_t mipLevel = 0);

        void capture(caustica::rhi::CommandList* commandList, dm::uint2 pixelPosition);

        dm::float4 readFloats();
        dm::uint4 readUInts();
        dm::int4 readInts();
    };
}