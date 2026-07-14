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

DLSS::DLSS(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory)
    : m_device(device)
{
    m_exposureShader = shaderFactory.createAutoShader("engine/passes/dlss_exposure_cs.hlsl", "main",
        CAUSTICA_MAKE_PLATFORM_SHADER(g_dlss_exposure_cs), nullptr, nvrhi::ShaderType::Compute);

    auto layoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::TypedBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float)));

    m_exposureBindingLayout = device->createBindingLayout(layoutDesc);

    auto pipelineDesc = nvrhi::ComputePipelineDesc()
        .addBindingLayout(m_exposureBindingLayout)
        .setComputeShader(m_exposureShader);

    m_exposurePipeline = device->createComputePipeline(pipelineDesc);

    auto textureDesc = nvrhi::TextureDesc()
        .setWidth(1)
        .setHeight(1)
        .setFormat(nvrhi::Format::R32_FLOAT)
        .setDebugName("DLSS Exposure Texture")
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDimension(nvrhi::TextureDimension::Texture2D)
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

void DLSS::computeExposure(nvrhi::ICommandList* commandList, nvrhi::IBuffer* toneMapperExposureBuffer, float exposureScale)
{
    if (m_exposureSourceBuffer != toneMapperExposureBuffer)
    {
        m_exposureSourceBuffer = nullptr;
        m_exposureBindingSet = nullptr;
    }

    if (!m_exposureBindingSet)
    {
        auto setDesc = nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::TypedBuffer_SRV(0, toneMapperExposureBuffer))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(0, m_exposureTexture))
            .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(float)));

        m_exposureBindingSet = m_device->createBindingSet(setDesc, m_exposureBindingLayout);
    }

    auto state = nvrhi::ComputeState()
        .setPipeline(m_exposurePipeline)
        .addBindingSet(m_exposureBindingSet);

    commandList->setComputeState(state);
    commandList->setPushConstants(&exposureScale, sizeof(float));
    commandList->dispatch(1);
}

 std::unique_ptr<DLSS> DLSS::create(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    switch(device->getGraphicsAPI())
    {
    case nvrhi::GraphicsAPI::D3D11:
        #if CAUSTICA_WITH_DX11
        return DLSS::createDX11(device, shaderFactory, directoryWithExecutable, applicationID);
        #else
        return nullptr;
        #endif
    case nvrhi::GraphicsAPI::D3D12:
        #if CAUSTICA_WITH_DX12
        return DLSS::createDX12(device, shaderFactory, directoryWithExecutable, applicationID);
        #else
        return nullptr;
        #endif
    case nvrhi::GraphicsAPI::VULKAN:
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
