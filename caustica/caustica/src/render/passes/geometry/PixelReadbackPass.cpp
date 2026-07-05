#include <render/passes/geometry/PixelReadbackPass.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/pixel_readback_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/pixel_readback_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/pixel_readback_cs.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/pixel_readback_cb.h>

using namespace caustica;
using namespace caustica::render;

PixelReadbackPass::PixelReadbackPass(
    nvrhi::IDevice* device, 
    std::shared_ptr<ShaderFactory> shaderFactory, 
    nvrhi::ITexture* inputTexture, 
    nvrhi::Format format,
    uint32_t arraySlice,
    uint32_t mipLevel)
    : m_device(device)
{
    const char* formatName = "";
    switch (format)
    {
    case nvrhi::Format::RGBA32_FLOAT: formatName = "float4"; break;
    case nvrhi::Format::RGBA32_UINT: formatName = "uint4"; break;
    case nvrhi::Format::RGBA32_SINT: formatName = "int4"; break;
    default: assert(!"unsupported readback format");
    }

    std::vector<ShaderMacro> macros;
    macros.push_back(ShaderMacro("TYPE", formatName));
    macros.push_back(ShaderMacro("INPUT_MSAA", inputTexture->getDesc().sampleCount > 1 ? "1" : "0"));
    m_Shader = shaderFactory->CreateAutoShader("engine/passes/pixel_readback_cs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_pixel_readback_cs), &macros, nvrhi::ShaderType::Compute);

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = 16;
    bufferDesc.format = format;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
    bufferDesc.keepInitialState = true;
    bufferDesc.debugName = "PixelReadbackPass/IntermediateBuffer";
    bufferDesc.canHaveTypedViews = true;
    m_IntermediateBuffer = m_device->createBuffer(bufferDesc);

    bufferDesc.canHaveUAVs = false;
    bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    bufferDesc.debugName = "PixelReadbackPass/ReadbackBuffer";
    m_ReadbackBuffer = m_device->createBuffer(bufferDesc);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(PixelReadbackConstants);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.debugName = "PixelReadbackPass/Constants";
    constantBufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
    m_ConstantBuffer = m_device->createBuffer(constantBufferDesc);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindings = { 
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(0)
    };

    m_BindingLayout = m_device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer),
        nvrhi::BindingSetItem::Texture_SRV(0, inputTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mipLevel, 1, arraySlice, 1)),
        nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_IntermediateBuffer)
    };

    m_BindingSet = m_device->createBindingSet(setDesc, m_BindingLayout);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_BindingLayout };
    pipelineDesc.CS = m_Shader;
    m_Pipeline = m_device->createComputePipeline(pipelineDesc);
}


void PixelReadbackPass::Capture(nvrhi::ICommandList* commandList, dm::uint2 pixelPosition)
{
    PixelReadbackConstants constants = {};
    constants.pixelPosition = dm::int2(pixelPosition);
    commandList->writeBuffer(m_ConstantBuffer, &constants, sizeof(constants));

    nvrhi::ComputeState state;
    state.pipeline = m_Pipeline;
    state.bindings = { m_BindingSet };
    commandList->setComputeState(state);
    commandList->dispatch(1, 1, 1);

    commandList->copyBuffer(m_ReadbackBuffer, 0, m_IntermediateBuffer, 0, m_ReadbackBuffer->getDesc().byteSize);
}

dm::float4 PixelReadbackPass::ReadFloats()
{
    void* pData = m_device->mapBuffer(m_ReadbackBuffer, nvrhi::CpuAccessMode::Read);
    assert(pData);

    float4 values = *static_cast<float4*>(pData);

    m_device->unmapBuffer(m_ReadbackBuffer);
    return values;
}

dm::uint4 PixelReadbackPass::ReadUInts()
{
    void* pData = m_device->mapBuffer(m_ReadbackBuffer, nvrhi::CpuAccessMode::Read);
    assert(pData);

    uint4 values = *static_cast<uint4*>(pData);

    m_device->unmapBuffer(m_ReadbackBuffer);
    return values;
}

dm::int4 PixelReadbackPass::ReadInts()
{
    void* pData = m_device->mapBuffer(m_ReadbackBuffer, nvrhi::CpuAccessMode::Read);
    assert(pData);

    int4 values = *static_cast<int4*>(pData);

    m_device->unmapBuffer(m_ReadbackBuffer);
    return values;
}
