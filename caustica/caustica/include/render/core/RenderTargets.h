#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <rhi/utils.h>
#include <rhi/common/misc.h>
#include <memory>
#include <render/core/GBuffer.h>

#include <shaders/PathTracer/Config.h>

namespace caustica
{
    class FramebufferFactory;
}

class RenderTargets// : public caustica::render::GBufferRenderTargets
{
    const dm::uint m_sampleCount = 1; // no MSAA supported in this sample
    bool m_useReverseProjection = false;
    int m_backbufferCount = -1;
    nvrhi::IDevice* m_device;
public:
    nvrhi::TextureHandle accumulatedRadiance;   // used only in non-realtime mode
    nvrhi::TextureHandle ldrColor;              // final, post-tonemapped color
    nvrhi::TextureHandle ldrColorScratch;       // used for ping-ponging post-process stuff vs LdrColor
    nvrhi::TextureHandle outputColor;           // raw path tracing output goes here (in both realtime and non-realtime modes); this can be input to TAA/DLSS
    nvrhi::TextureHandle processedOutputColor;  // for when post-processing OutputColor (i.e. TAA) (previously ResolvedColor); this is the output of TAA/DLSS in full res, but before tonemapping and without ImGUI
    nvrhi::TextureHandle temporalFeedback1;     // used by TAA
    nvrhi::TextureHandle temporalFeedback2;     // used by TAA
    nvrhi::TextureHandle preUIColor;            // used DLSS-G

    // note: DLSS-RR also uses ProcessedOutputColor as sl::kBufferTypeScalingOutputColor (-RR output) and OutputColor as sl::kBufferTypeScalingInputColor (-RR input)
    nvrhi::TextureHandle rrDiffuseAlbedo;       // used by DLSS-RR, see: sl::kBufferTypeAlbedo
    nvrhi::TextureHandle rrSpecAlbedo;          // used by DLSS-RR, see: sl::kBufferTypeSpecularAlbedo
    nvrhi::TextureHandle rrNormalsAndRoughness; // used by DLSS-RR, see: sl::kBufferTypeNormals and
    nvrhi::TextureHandle rrSpecMotionVectors;   // used by DLSS-RR, see: sl::kBufferTypeSpecularMotionVectors and sl::DLSSDNormalRoughnessMode::ePacked
    nvrhi::TextureHandle rrTransparencyLayer;   // used by DLSS-RR, see: sl::kBufferTypeTransparencyLayer

    nvrhi::TextureHandle throughput;            // when using PSR we need to remember throughput after perfect speculars with color for RTXDI to know how to do its thing correctly
    nvrhi::TextureHandle depth;                 // exported by path tracer, used by TAA and others
    nvrhi::TextureHandle screenMotionVectors;   // screen space motion vectors, exported by path tracer, used by RTXDI, TAA and others

    nvrhi::TextureHandle denoiserViewspaceZ;
    nvrhi::TextureHandle denoiserMotionVectors;
    nvrhi::TextureHandle denoiserNormalRoughness;

    nvrhi::TextureHandle stableRadiance;                    // radiance that doesn't require denoising; this is technically not needed as a separate buffer, but very useful for debug viz
    nvrhi::TextureHandle stablePlanesHeader;
    nvrhi::BufferHandle  stablePlanesBuffer;

    nvrhi::BufferHandle  surfaceDataBuffer;

    nvrhi::TextureHandle denoiserAvgLayerRadianceHalfRes;

    nvrhi::TextureHandle denoiserDiffRadianceHitDist;       // input to denoiser
    nvrhi::TextureHandle denoiserSpecRadianceHitDist;       // input to denoiser
    nvrhi::TextureHandle denoiserDisocclusionThresholdMix;  // input to denoiser (see IN_DISOCCLUSION_THRESHOLD_MIX)
    
    nvrhi::TextureHandle combinedHistoryClampRelax;         // all DenoiserDisocclusionThresholdMix combined together - used to tell TAA where to relax disocclusion test to minimize aliasing

    nvrhi::TextureHandle denoiserOutDiffRadianceHitDist[cStablePlaneCount]; // output from denoiser, texture per denoiser instance - search for OUT_DIFF_RADIANCE_HITDIST in NRDDescs.h for more info
    nvrhi::TextureHandle denoiserOutSpecRadianceHitDist[cStablePlaneCount]; // output from denoiser, texture per denoiser instance - search for OUT_SPEC_RADIANCE_HITDIST in NRDDescs.h for more info
    nvrhi::TextureHandle denoiserOutValidation = nullptr;   // output from denoiser (for validation) - leave nullptr to disable validation

    nvrhi::TextureHandle secondarySurfacePositionNormal;    // input to restir gi
    nvrhi::TextureHandle secondarySurfaceRadiance;          // input to restir gi

    // GBuffer
    nvrhi::TextureHandle baseColor;
    nvrhi::TextureHandle specNormal;
    nvrhi::TextureHandle roughnessMetal;
    nvrhi::TextureHandle materialInfo;

    nvrhi::TextureHandle specularHitT;                      // input for denoisers to be able to resolve spec motion vectors
    nvrhi::TextureHandle scratchFloat1;                     // can be used to ping-pong stuff - 32bit float 1

    // === Reflection System render Targets ===
    nvrhi::TextureHandle localCubemap;                      // 256x256x6, RGBA16F, ray-traced local environment cubemap
    nvrhi::TextureHandle ssrResult;                         // Screen-sized, RGBA16F (rgb=reflection, a=confidence)
    nvrhi::TextureHandle ssrBlurMipChain;                   // Screen-sized with mip levels for roughness blur

    static constexpr uint32_t LocalCubemapSize = 256;        // Size of local environment cubemap faces
    static constexpr uint32_t SSRMaxMipLevels = 13;          // Number of blur mip levels for SSR

    nvrhi::HeapHandle heap;

    dm::uint2 renderSize;// size of render targets pre-DLSS
    dm::uint2 displaySize; // size of render targets post-DLSS

    // Framebuffers are used by the bloom and tone mapping passes
    std::shared_ptr<caustica::FramebufferFactory> outputFramebuffer;
    std::shared_ptr<caustica::FramebufferFactory> processedOutputFramebuffer;
    std::shared_ptr<caustica::FramebufferFactory> ldrFramebuffer;

    void init(nvrhi::IDevice* device, dm::uint2 renderSize, dm::uint2 displaySize, bool enableMotionVectors, bool useReverseProjection, int backbufferCount);// override;
    [[nodiscard]] bool isUpdateRequired(dm::uint2 renderSize, dm::uint2 displaySize, dm::uint sampleCount = 1) const;
    void clear(nvrhi::ICommandList* commandList);

    static uint32_t getNumMipLevels(uint32_t width, uint32_t height);
};

