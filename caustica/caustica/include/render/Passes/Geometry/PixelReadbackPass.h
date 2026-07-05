#pragma once

#include <math/math.h>
#include <memory>
#include <map>
#include <rhi/nvrhi.h>


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
        nvrhi::DeviceHandle m_device;
        nvrhi::ShaderHandle m_Shader;
        nvrhi::ComputePipelineHandle m_Pipeline;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::BufferHandle m_ConstantBuffer;
        nvrhi::BufferHandle m_IntermediateBuffer;
        nvrhi::BufferHandle m_ReadbackBuffer;

    public:
        PixelReadbackPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            nvrhi::ITexture* inputTexture,
            nvrhi::Format format,
            uint32_t arraySlice = 0,
            uint32_t mipLevel = 0);

        void Capture(nvrhi::ICommandList* commandList, dm::uint2 pixelPosition);

        dm::float4 ReadFloats();
        dm::uint4 ReadUInts();
        dm::int4 ReadInts();
    };
}