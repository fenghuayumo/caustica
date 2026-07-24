#pragma once

#include <math/math.h>
#include <rhi/rhi.h>
#include <rhi/utils.h>
#include <rhi/common/misc.h>
#include <memory>

#include <shaders/PathTracer/Config.h>

namespace caustica
{
    class FramebufferFactory;
}

class RenderTargets
{
    const dm::uint m_sampleCount = 1; // no MSAA supported in this sample
    bool m_useReverseProjection = false;
    int m_backbufferCount = -1;
    caustica::rhi::Device* m_device;
public:
    caustica::rhi::TextureHandle accumulatedRadiance;   // used only in non-realtime mode
    caustica::rhi::TextureHandle ldrColor;              // final, post-tonemapped color
    caustica::rhi::TextureHandle ldrColorScratch;       // used for ping-ponging post-process stuff vs LdrColor
    caustica::rhi::TextureHandle outputColor;           // raw path tracing output goes here (in both realtime and non-realtime modes); this can be input to TAA/DLSS
    caustica::rhi::TextureHandle processedOutputColor;  // for when post-processing OutputColor (i.e. TAA) (previously ResolvedColor); this is the output of TAA/DLSS in full res, but before tonemapping and without ImGUI
    caustica::rhi::TextureHandle temporalFeedback1;     // used by TAA
    caustica::rhi::TextureHandle temporalFeedback2;     // used by TAA
    caustica::rhi::TextureHandle preUIColor;            // used DLSS-G

    // note: DLSS-RR also uses ProcessedOutputColor as sl::kBufferTypeScalingOutputColor (-RR output) and OutputColor as sl::kBufferTypeScalingInputColor (-RR input)
    caustica::rhi::TextureHandle rrDiffuseAlbedo;       // used by DLSS-RR, see: sl::kBufferTypeAlbedo
    caustica::rhi::TextureHandle rrSpecAlbedo;          // used by DLSS-RR, see: sl::kBufferTypeSpecularAlbedo
    caustica::rhi::TextureHandle rrNormalsAndRoughness; // used by DLSS-RR, see: sl::kBufferTypeNormals and
    caustica::rhi::TextureHandle rrSpecMotionVectors;   // used by DLSS-RR, see: sl::kBufferTypeSpecularMotionVectors and sl::DLSSDNormalRoughnessMode::ePacked
    caustica::rhi::TextureHandle rrTransparencyLayer;   // used by DLSS-RR, see: sl::kBufferTypeTransparencyLayer

    caustica::rhi::TextureHandle throughput;            // when using PSR we need to remember throughput after perfect speculars with color for RTXDI to know how to do its thing correctly
    caustica::rhi::TextureHandle depth;                 // exported by path tracer, used by TAA and others
    caustica::rhi::TextureHandle screenMotionVectors;   // screen space motion vectors, exported by path tracer, used by RTXDI, TAA and others

    caustica::rhi::TextureHandle denoiserViewspaceZ;
    caustica::rhi::TextureHandle denoiserMotionVectors;
    caustica::rhi::TextureHandle denoiserNormalRoughness;

    caustica::rhi::TextureHandle stableRadiance;                    // radiance that doesn't require denoising; this is technically not needed as a separate buffer, but very useful for debug viz
    caustica::rhi::TextureHandle stablePlanesHeader;
    caustica::rhi::BufferHandle  stablePlanesBuffer;

    caustica::rhi::BufferHandle  surfaceDataBuffer;

    caustica::rhi::TextureHandle denoiserAvgLayerRadianceHalfRes;

    caustica::rhi::TextureHandle denoiserDiffRadianceHitDist;       // input to denoiser
    caustica::rhi::TextureHandle denoiserSpecRadianceHitDist;       // input to denoiser
    caustica::rhi::TextureHandle denoiserDisocclusionThresholdMix;  // input to denoiser (see IN_DISOCCLUSION_THRESHOLD_MIX)
    
    caustica::rhi::TextureHandle combinedHistoryClampRelax;         // all DenoiserDisocclusionThresholdMix combined together - used to tell TAA where to relax disocclusion test to minimize aliasing

    caustica::rhi::TextureHandle denoiserOutDiffRadianceHitDist[cStablePlaneCount]; // output from denoiser, texture per denoiser instance - search for OUT_DIFF_RADIANCE_HITDIST in NRDDescs.h for more info
    caustica::rhi::TextureHandle denoiserOutSpecRadianceHitDist[cStablePlaneCount]; // output from denoiser, texture per denoiser instance - search for OUT_SPEC_RADIANCE_HITDIST in NRDDescs.h for more info
    caustica::rhi::TextureHandle denoiserOutValidation = nullptr;   // output from denoiser (for validation) - leave nullptr to disable validation

    caustica::rhi::TextureHandle secondarySurfacePositionNormal;    // input to restir gi
    caustica::rhi::TextureHandle secondarySurfaceRadiance;          // input to restir gi

    // GBuffer
    caustica::rhi::TextureHandle baseColor;
    caustica::rhi::TextureHandle specNormal;
    caustica::rhi::TextureHandle roughnessMetal;
    caustica::rhi::TextureHandle materialInfo;

    caustica::rhi::TextureHandle specularHitT;                      // input for denoisers to be able to resolve spec motion vectors
    caustica::rhi::TextureHandle scratchFloat1;                     // can be used to ping-pong stuff - 32bit float 1

    // === Reflection System render Targets ===
    caustica::rhi::TextureHandle localCubemap;                      // 256x256x6, RGBA16F, ray-traced local environment cubemap
    caustica::rhi::TextureHandle ssrResult;                         // Screen-sized, RGBA16F (rgb=reflection, a=confidence)
    caustica::rhi::TextureHandle ssrBlurMipChain;                   // Screen-sized with mip levels for roughness blur

    static constexpr uint32_t LocalCubemapSize = 256;        // Size of local environment cubemap faces
    static constexpr uint32_t SSRMaxMipLevels = 13;          // Number of blur mip levels for SSR

    caustica::rhi::HeapHandle heap;

    dm::uint2 renderSize;// size of render targets pre-DLSS
    dm::uint2 displaySize; // size of render targets post-DLSS

    // Framebuffers are used by the bloom and tone mapping passes
    std::shared_ptr<caustica::FramebufferFactory> outputFramebuffer;
    std::shared_ptr<caustica::FramebufferFactory> processedOutputFramebuffer;
    std::shared_ptr<caustica::FramebufferFactory> ldrFramebuffer;

    void init(caustica::rhi::Device* device, dm::uint2 renderSize, dm::uint2 displaySize, bool enableMotionVectors, bool useReverseProjection, int backbufferCount);// override;
    [[nodiscard]] bool isUpdateRequired(dm::uint2 renderSize, dm::uint2 displaySize, dm::uint sampleCount = 1) const;
    void clear(caustica::rhi::CommandList* commandList);

    static uint32_t getNumMipLevels(uint32_t width, uint32_t height);
};

