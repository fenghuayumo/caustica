#if CAUSTICA_WITH_DLSS

#include <render/passes/geometry/DLSS.h>
#include <assets/loader/ShaderFactory.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/dlss_exposure_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/dlss_exposure_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/dlss_exposure_cs.spirv.h"
#endif
#endif

using namespace caustica::render;

DLSS::DLSS(caustica::rhi::Device* device, caustica::ShaderFactory& shaderFactory)
    : m_device(device)
{
    m_exposureShader = shaderFactory.createAutoShader("engine/passes/dlss_exposure_cs.hlsl", "main",
        CAUSTICA_MAKE_PLATFORM_SHADER(g_dlss_exposure_cs), nullptr, caustica::rhi::ShaderType::Compute);

    auto layoutDesc = caustica::rhi::BindingLayoutDesc()
        .setVisibility(caustica::rhi::ShaderType::Compute)
        .addItem(caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(0))
        .addItem(caustica::rhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(float)));

    m_exposureBindingLayout = device->createBindingLayout(layoutDesc);

    auto pipelineDesc = caustica::rhi::ComputePipelineDesc()
        .addBindingLayout(m_exposureBindingLayout)
        .setComputeShader(m_exposureShader);

    m_exposurePipeline = device->createComputePipeline(pipelineDesc);

    auto textureDesc = caustica::rhi::TextureDesc()
        .setWidth(1)
        .setHeight(1)
        .setFormat(caustica::rhi::Format::R32_FLOAT)
        .setDebugName("DLSS Exposure Texture")
        .setInitialState(caustica::rhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDimension(caustica::rhi::TextureDimension::Texture2D)
        .setIsUAV(true);

    m_exposureTexture = device->createTexture(textureDesc);

    m_featureCommandList = device->createCommandList();
}

bool DLSS::isDlssSupported() const
{
    return m_dlssSupported;
}

bool DLSS::isDlssInitialized() const
{
    return m_dlssInitialized;
}

bool DLSS::isRayReconstructionSupported() const
{
    return m_rayReconstructionSupported;
}

bool DLSS::isRayReconstructionInitialized() const
{
    return m_rayReconstructionInitialized;
}

void DLSS::computeExposure(caustica::rhi::CommandList* commandList, caustica::rhi::Buffer* toneMapperExposureBuffer, float exposureScale)
{
    if (m_exposureSourceBuffer != toneMapperExposureBuffer)
    {
        m_exposureSourceBuffer = nullptr;
        m_exposureBindingSet = nullptr;
    }

    if (!m_exposureBindingSet)
    {
        auto setDesc = caustica::rhi::BindingSetDesc()
            .addItem(caustica::rhi::BindingSetItem::TypedBuffer_SRV(0, toneMapperExposureBuffer))
            .addItem(caustica::rhi::BindingSetItem::Texture_UAV(0, m_exposureTexture))
            .addItem(caustica::rhi::BindingSetItem::PushConstants(0, sizeof(float)));

        m_exposureBindingSet = m_device->createBindingSet(setDesc, m_exposureBindingLayout);
    }

    auto state = caustica::rhi::ComputeState()
        .setPipeline(m_exposurePipeline)
        .addBindingSet(m_exposureBindingSet);

    commandList->setComputeState(state);
    commandList->setPushConstants(&exposureScale, sizeof(float));
    commandList->dispatch(1);
}

 std::unique_ptr<DLSS> DLSS::create(caustica::rhi::Device* device, caustica::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    switch(device->getGraphicsAPI())
    {
    case caustica::rhi::GraphicsAPI::D3D11:
        #if CAUSTICA_WITH_DX11
        return DLSS::createDX11(device, shaderFactory, directoryWithExecutable, applicationID);
        #else
        return nullptr;
        #endif
    case caustica::rhi::GraphicsAPI::D3D12:
        #if CAUSTICA_WITH_DX12
        return DLSS::createDX12(device, shaderFactory, directoryWithExecutable, applicationID);
        #else
        return nullptr;
        #endif
    case caustica::rhi::GraphicsAPI::VULKAN:
        #if CAUSTICA_WITH_VULKAN
        return DLSS::createVK(device, shaderFactory, directoryWithExecutable, applicationID);
        #else
        return nullptr;
        #endif
    default:
        return nullptr;
    }
}
#endif
