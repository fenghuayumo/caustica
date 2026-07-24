#pragma once

#include <render/core/BindingCache.h>
#include <rhi/rhi.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderDevice.h>
#include <math/math.h>
#include <memory>

using namespace caustica::math;

#include <render/core/RenderTargets.h>
#include <shaders/SampleConstantBuffer.h>

namespace caustica
{
    class FramebufferFactory;
}

class ShaderDebug;

// TODO: either replace this whole approach with instances of ComputePass OR replace internal m_computeengine/shaders/m_computePSOs with ComputePass
class PostProcess 
{
public:
    enum class ComputePassType
    {
        StablePlanesDebugViz,
        RELAXDenoiserPrepareInputs,
        REBLURDenoiserPrepareInputs,
        RELAXDenoiserFinalMerge,
        REBLURDenoiserFinalMerge,
        DLSSRRDenoiserPrepareInputs,
        NoDenoiserFinalMerge,
        DummyPlaceholder,

        MaxCount
    };

private:

    caustica::rhi::DeviceHandle             m_device;
    caustica::render::RenderDevice& m_renderDevice;

    caustica::rhi::ShaderHandle             m_computeShaders[(uint32_t)ComputePassType::MaxCount];
    caustica::rhi::ComputePipelineHandle    m_computePSOs[(uint32_t)ComputePassType::MaxCount];

    caustica::rhi::SamplerHandle            m_pointSampler;
    caustica::rhi::SamplerHandle            m_linearSampler;

    caustica::rhi::BindingLayoutHandle      m_bindingLayoutCS;
    caustica::rhi::BindingSetHandle         m_bindingSetCS;

    caustica::BindingCache     m_bindingCache;

    std::shared_ptr<ShaderDebug>    m_shaderDebug;

public:

    PostProcess(
        caustica::rhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        caustica::render::RenderDevice& renderDevice,
        std::shared_ptr<ShaderDebug> shaderDebug
        //, std::shared_ptr<caustica::FramebufferFactory> colorFramebufferFactory
    );

    void apply(caustica::rhi::ICommandList* commandList, ComputePassType passType, caustica::rhi::BufferHandle consts, SampleMiniConstants & miniConsts, caustica::rhi::BindingSetHandle bindingSet, caustica::rhi::BindingLayoutHandle bindingLayout, uint32_t width, uint32_t height);
    void apply(caustica::rhi::ICommandList* commandList, ComputePassType passType, int pass, caustica::rhi::BufferHandle consts, SampleMiniConstants & miniConsts, caustica::rhi::ITexture* workTexture, RenderTargets & renderTargets, caustica::rhi::ITexture* sourceTexture);
};
