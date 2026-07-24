#pragma once

#include <rhi/rhi.h>
#include <memory>
#include <unordered_map>
#include <string>
#include <math/math.h>
#include <render/passes/geometry/MipMapGenPass.h>
#include <render/graph/GraphBuilder.h>
#include <render/core/RenderDevice.h>
#include <render/core/ToneMappingParameters.h>

using caustica::math::float3;
using caustica::math::float3x3;

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

#ifndef TONEMAPPING_AUTOEXPOSURE_CPU
#error this must be defined
#endif

static const std::unordered_map<ToneMapperOperator, std::string> tonemapOperatorToString = {
    {ToneMapperOperator::Linear, "Linear"},
    {ToneMapperOperator::Reinhard, "Reinhard"},
    {ToneMapperOperator::ReinhardModified, "Reinhard Modified"},
    {ToneMapperOperator::HejiHableAlu, "Heji Hable ALU"},
    {ToneMapperOperator::HableUc2, "Hable UC2"},
    {ToneMapperOperator::Aces, "Aces"}
};

class ToneMappingPass
{
private:

    caustica::rhi::DeviceHandle m_device;
    caustica::rhi::ShaderHandle m_LuminanceShader;
    caustica::rhi::ShaderHandle m_ToneMapShader;

    struct PerViewData
    {
        caustica::rhi::TextureHandle luminanceTexture;
		caustica::rhi::FramebufferHandle luminanceFrameBuffer;
        std::unique_ptr<caustica::render::MipMapGenPass> mipMapPass;
        caustica::rhi::BindingSetHandle luminanceBindingSet;
        caustica::rhi::BindingSetHandle colorBindingSet;
        caustica::rhi::TextureHandle sourceTexture;

#if TONEMAPPING_AUTOEXPOSURE_CPU
        // used for readback
        static constexpr int cReadbackLag = 3;  // if used once per frame then it should be backbuffer (swapchain) count + 1 to ensure it never blocks
        caustica::rhi::BufferHandle avgLuminanceBufferGPU;
        caustica::rhi::BufferHandle avgLuminanceBufferReadback[cReadbackLag];
        int                 avgLuminanceLastWritten     = -1;
        float               avgLuminanceLastCaptured    = 0.0;
#endif
    };
        
    std::vector<PerViewData> m_PerView;

    caustica::rhi::BufferHandle m_ToneMappingCB;

    caustica::rhi::SamplerHandle m_linearSampler;
    caustica::rhi::SamplerHandle m_pointSampler;

    float m_FrameTime = 0.f;

	caustica::rhi::BindingLayoutHandle m_LuminanceBindingLayout;
	caustica::rhi::GraphicsPipelineHandle m_LuminancePso;

    caustica::rhi::BindingLayoutHandle m_ToneMapBindingLayout;
    caustica::rhi::GraphicsPipelineHandle m_ToneMapPso;
#if TONEMAPPING_AUTOEXPOSURE_CPU
    caustica::rhi::ShaderHandle m_CaptureLuminanceShader;
    caustica::rhi::BindingLayoutHandle m_CaptureLumBindingLayout;
    caustica::rhi::ComputePipelineHandle m_CaptureLumPso;
#endif

    caustica::render::RenderDevice& m_renderDevice;
    std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;
        
    ExposureMode m_ExposureMode;
    ToneMapperOperator m_ToneMapOperator;
    bool m_AutoExposure;
    float m_ExposureCompensation;
    float m_ExposureValue;
    float m_ExposureValueMin;
    float m_ExposureValueMax;
    float m_FilmSpeed;        
    float m_FNumber;
    float m_Shutter;
        
    bool m_WhiteBalance;
    float m_WhitePoint;
    float m_WhiteMaxLuminance;
    float m_WhiteScale;
    int m_Clamped;
        
    //Pre-computed fields
    float3x3 m_WhiteBalanceTransform; 
    float3 m_SourceWhite;
    float3x3 m_ColorTransform; 
        
    bool m_FrameParamsSet = false;

    void setParameters(const ToneMappingParameters& params);
    void updateExposureValue();
	void updateWhiteBalanceTransform();
	void updateColorTransform();
    void generateMips(caustica::rhi::ICommandList* commandList, uint32_t numberOfViews);
public:
    struct CreateParameters
    {
        bool isTextureArray = false;
        uint32_t histogramBins = 256;
        uint32_t numConstantBufferVersions = 16;
        caustica::rhi::IBuffer* exposureBufferOverride = nullptr;
        caustica::rhi::ITexture* colorLUT = nullptr;
    };

    ToneMappingPass(
        caustica::rhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        caustica::render::RenderDevice& renderDevice,
        std::shared_ptr<caustica::FramebufferFactory> colorFramebufferFactory,
        const caustica::ICompositeView& compositeView,
        caustica::rhi::TextureHandle sourceTexture
        );

    void preRender(const ToneMappingParameters& params);

    // note - if enable == false, it still does autoexposure (if enabled) and everything else, but the output is just passthrough
    bool render(caustica::rhi::ICommandList* commandList,
        const caustica::ICompositeView& compositeView,
        caustica::rhi::ITexture* sourceTexture,
        caustica::rhi::IBuffer* constantsBuffer,
        bool enabled);

    // R1 pilot — registers ToneMapping as a render-graph pass with automatic barriers.
    void registerGraphPass(
        caustica::rg::GraphBuilder& graph,
        caustica::rg::TextureHandle sourceColor,
        caustica::rg::TextureHandle outputLdrColor,
        const caustica::ICompositeView& compositeView,
        bool enabled,
        bool* outCommandListWasClosed = nullptr);

#if TONEMAPPING_AUTOEXPOSURE_CPU
    float3 getPreExposedGray( uint viewIndex );
#endif

    void advanceFrame(float frameTime);

    caustica::rhi::TextureHandle getLuminanceTexture(uint viewIndex)    { return m_PerView[viewIndex].luminanceTexture; }
};
//}
