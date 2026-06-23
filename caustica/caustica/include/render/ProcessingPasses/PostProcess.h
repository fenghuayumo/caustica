#pragma once

#include <engine/BindingCache.h>
#include <rhi/nvrhi.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/CommonRenderPasses.h>
#include <math/math.h>
#include <memory>

using namespace caustica::math;

#include <SampleCommon/RenderTargets.h>
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

    nvrhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;

    nvrhi::ShaderHandle             m_computeShaders[(uint32_t)ComputePassType::MaxCount];
    nvrhi::ComputePipelineHandle    m_computePSOs[(uint32_t)ComputePassType::MaxCount];

    nvrhi::SamplerHandle            m_pointSampler;
    nvrhi::SamplerHandle            m_linearSampler;

    nvrhi::BindingLayoutHandle      m_bindingLayoutCS;
    nvrhi::BindingSetHandle         m_bindingSetCS;

    caustica::BindingCache     m_bindingCache;

    std::shared_ptr<ShaderDebug>    m_shaderDebug;

public:

    PostProcess(
        nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        std::shared_ptr<caustica::CommonRenderPasses> commonPasses,
        std::shared_ptr<ShaderDebug> shaderDebug
        //, std::shared_ptr<caustica::FramebufferFactory> colorFramebufferFactory
    );

    void Apply(nvrhi::ICommandList* commandList, ComputePassType passType, nvrhi::BufferHandle consts, SampleMiniConstants & miniConsts, nvrhi::BindingSetHandle bindingSet, nvrhi::BindingLayoutHandle bindingLayout, uint32_t width, uint32_t height);
    void Apply(nvrhi::ICommandList* commandList, ComputePassType passType, int pass, nvrhi::BufferHandle consts, SampleMiniConstants & miniConsts, nvrhi::ITexture* workTexture, RenderTargets & renderTargets, nvrhi::ITexture* sourceTexture);
};
