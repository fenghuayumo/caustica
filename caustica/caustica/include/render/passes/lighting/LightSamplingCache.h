#pragma once

#include <render/core/BindingCache.h>
#include <render/SceneGpuResources.h>
#include <rhi/rhi.h>
#include <math/math.h>
#include <memory>
#include <vector>

#include <math/math.h>

#define NEEAT_BAKER_ONLY 1
#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>

#include <render/core/ComputePass.h>

#include <shaders/SubInstanceData.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <scene/SceneRenderData.h>

#include <filesystem>
#include <unordered_map>

using namespace caustica::math;

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render { class RenderDevice; }
    struct ImageAsset;
}

class ShaderDebug;

// Render-thread light history is keyed by entity / instance hash.
// Do not mutate *LightComponent::lightLink or MeshInstanceComponent::perGeometryLightSamplerLinks.

// This prepares all scene lighting (including environment map already partially processed by EnvMapProcessor) for sampling in path tracing.
// Supported sampling approaches are Uniform, Power and NEE-AT. All NEE-AT baking logic is included here.
class LightSamplingCache 
{
public:
    struct UpdateSettings
    {
        uint        ImportanceSamplingType      = 0;                    // 0 - uniform; 1 - pure power based; 2 - NEE-AT
        float3      CameraPosition              = float3(0,0,0);
        float3      CameraDirection             = float3(0,0,0);
        float       AverageContentsDistance     = 10.0f;                // rough average distance from camera that most viewed objects will be at - 1-100m is good for FPS, could be 1000 for a flight sim
        uint2       MouseCursorPos              = uint2(0,0);           // only used for debug viz
        float4x4    ViewProjMatrix              = float4x4::identity(); // needed for figuring out frustum planes for optimizations
        
        // NEE-AT settings
        float       GlobalTemporalFeedbackWeight    = 0.75f;    // 0.0 - use no feedback for global sampler, 0.95 use almost feedback only (some power-based input always needed to bring in new lights)
        float       LocalToGlobalSampleRatio        = 0.65f;    // 0.0 - sample exclusively from global sampler, 1.0 sample (almost) exclusively from local sampler
        // float       LightSampling_MIS_Boost         = 1.0f;     // boost light sampling when doing MIS vs BSDF <- TODO: redesign, make it work with 'UseApproximateMIS'
        bool        UseApproximateMIS           = false;

        // frame/global settings
        bool        ResetFeedback = false;
        float2      ViewportSize                    = {0,0};
        float2      PrevViewportSize                = {0,0};

        // environment map parameters
        LightSamplingCacheEnvMapParams EnvMapParams        = {};
        float DistantVsLocalImportanceScale         = 1.0f;

        const std::vector<GaussianSplatEmissionProxy>* GaussianSplatEmissionProxies = nullptr;
        float4x4    GaussianSplatEmissionObjectToWorld = float4x4::identity();
        float       GaussianSplatEmissionIntensity = 0.0f;

        int64_t     FrameIndex                      = -1;
    };

public:
    LightSamplingCache(caustica::rhi::Device* device);
    ~LightSamplingCache();

    // reset scene related stuff
    void                            sceneReloaded();

    void                            createRenderPasses(std::shared_ptr<caustica::ShaderFactory> shaderFactory, caustica::rhi::BindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, std::shared_ptr<ShaderDebug> shaderDebug, const uint2 renderResolution, const uint envMapProcessedResolution);

    // Main and only processing stage is split into updateBegin/updateEnd. These can be called one after the other as soon as screen space motion vectors are available.
    // The split is purely to facilitate any potential async compute.
    
    // updateBegin can happen in parallel with any other ray preparatory tracing work - anything from BVH building to laying down denoising layers. Emissive triangle emission must be accessible at this point.
    void                            updateBegin(caustica::rhi::CommandList * commandList, caustica::BindingCache & bindingCache, const UpdateSettings & settings, double sceneTime, const caustica::scene::SceneRenderData* sceneData, const caustica::render::SceneGpuFrameHandles& gpuHandles, caustica::rhi::DescriptorTable* bindlessDescriptorTable, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, caustica::rhi::BufferHandle subInstanceDataBuffer, std::vector<SubInstanceData> & subInstanceData, caustica::rhi::TextureHandle envMapProcessed);
    // updateEnd must happen BEFORE any light sampling (e.g. PT pass with NEE) but AFTER screen space motion vectors are available for reprojection.
    void                            updateEnd(caustica::rhi::CommandList * commandList, caustica::BindingCache & bindingCache, const caustica::render::SceneGpuFrameHandles& gpuHandles, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, caustica::rhi::BufferHandle subInstanceDataBuffer, caustica::rhi::TextureHandle depthBuffer, caustica::rhi::TextureHandle motionVectors);

    caustica::rhi::BufferHandle             getControlBuffer() const                    { return m_controlBuffer; }
    caustica::rhi::BufferHandle             getLightBuffer() const                      { return m_lightsBuffer; }              // this is the list of lights
    caustica::rhi::BufferHandle             getLightExBuffer() const                    { return m_lightsExBuffer; }            // this is the list of light (extended data)
    caustica::rhi::BufferHandle             getLightProxyCounters() const               { return m_perLightProxyCounters; }     // these are counters of how many proxies each light has
    caustica::rhi::BufferHandle             getLightSamplingProxies() const             { return m_lightSamplingProxies; }      // these are indices into the getLightBuffer()

    caustica::rhi::TextureHandle            getEnvLightLookupMap() const                { return m_envLightLookupMap; }

    caustica::rhi::BufferHandle             getLocalSamplingBuffer() const              { return m_NEE_AT_LocalSamplingBuffer; }

    caustica::rhi::TextureHandle            getFeedbackTotalWeight() const              { return m_NEE_AT_FeedbackTotalWeight; }
    caustica::rhi::TextureHandle            getFeedbackCandidates() const               { return m_NEE_AT_FeedbackCandidates; }
    caustica::rhi::TextureHandle            getFeedbackTotalWeightScratch() const       { return m_NEE_AT_FeedbackTotalWeightScratch; }
    caustica::rhi::TextureHandle            getFeedbackCandidatesScratch() const        { return m_NEE_AT_FeedbackCandidatesScratch; }
    caustica::rhi::TextureHandle            getFeedbackTotalWeightBlended() const       { return m_NEE_AT_FeedbackTotalWeightBlended; }
    caustica::rhi::TextureHandle            getFeedbackCandidatesBlended() const        { return m_NEE_AT_FeedbackCandidatesBlended; }
    caustica::rhi::TextureHandle            getHistoryDepth() const                     { return m_NEE_AT_HistoryDepth; }


    bool                            infoGUI(float indent);
    bool                            debugGUI(float indent);

    void                            setGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros);
    [[nodiscard]] bool              sampleBakedEnvironment() const { return m_advSetting_SampleBakedEnvironment; }

    bool                            totalLightCountOverflow() const;

private:

    // output goes into m_scratchLightBuffer and 
    static bool                     collectEnvmapLightPlaceholders(const UpdateSettings & settings, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPastBuffer, std::vector<uint> & outLightHistoryRemapPastToCurrent);
    bool                            collectAnalyticLightsCPU(const UpdateSettings & settings, const caustica::scene::SceneRenderData& sceneData, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPast, std::vector<uint> & outLightHistoryRemapPastToCurrent);
    bool                            collectGaussianSplatEmissionProxies(const UpdateSettings & settings, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPast, std::vector<uint> & outLightHistoryRemapPastToCurrent);

    // this creates emissive triangle proc tasks and also does any required geometry instance (subInstance) processing such as analyt light proxies; has to happen AFTER collectAnalyticLightsCPU
    bool                            processEmissiveGeometry( const UpdateSettings & settings, const caustica::scene::SceneRenderData& sceneData, MaterialGpuCache& materialGpuCache, std::vector<SubInstanceData> & subInstanceData, LightingControlData & ctrlBuff, std::vector<struct EmissiveTrianglesProcTask> & tasks );

    void                            fillBindings(caustica::rhi::BindingSetDesc& outBindingSetDesc, const caustica::render::SceneGpuFrameHandles& gpuHandles, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, caustica::rhi::BufferHandle subInstanceDataBuffer, caustica::rhi::TextureHandle depthBuffer, caustica::rhi::TextureHandle motionVectors, caustica::rhi::TextureHandle envMapProcessed);

    void                            updateFrustumConsts(LightSamplingCacheConstants & outConsts, const LightSamplingCache::UpdateSettings & settings);

    void                            updateLocalJitter();

private:
    caustica::rhi::DeviceHandle             m_device;
    caustica::render::RenderDevice* m_renderDevice = nullptr;
    std::shared_ptr<caustica::FramebufferFactory> m_framebufferFactory;
    std::shared_ptr<ShaderDebug>    m_shaderDebug;

    ComputePass                     m_resetPastToCurrentHistory;

    ComputePass                     m_envLightsBackupPast;
    ComputePass                     m_envLightsSubdivideBase;
    ComputePass                     m_envLightsSubdivideBoost;
    ComputePass                     m_envLightsFillLookupMap;
    ComputePass                     m_envLightsMapPastToCurrent;

    ComputePass                     m_bakeEmissiveTriangles;

    ComputePass                     m_clearFeedbackHistory;
    ComputePass                     m_processFeedbackHistoryP0;
    ComputePass                     m_processFeedbackHistoryP1a;
    ComputePass                     m_processFeedbackHistoryP1b;
    ComputePass                     m_processFeedbackHistoryP2;
    ComputePass                     m_processFeedbackHistoryP3;
    ComputePass                     m_processFeedbackHistoryDebugViz;

    ComputePass                     m_processFeedbackHistoryPreFilter;

    ComputePass                     m_resetLightProxyCounters;
    ComputePass                     m_computeWeights;
    ComputePass                     m_computeProxyCounts;
    ComputePass                     m_computeProxyBaselineOffsets;
    ComputePass                     m_createProxyJobs;
    ComputePass                     m_executeProxyJobs;
    ComputePass                     m_debugDrawLights;
    
    caustica::rhi::BindingLayoutHandle      m_commonBindingLayout;

    UpdateSettings                    m_currentSettings;
    LightingControlData             m_currentCtrlBuff;              // NOTE: this does not include GPU-side changes, only the initial state set in update

    caustica::rhi::BufferHandle             m_controlBuffer;

    caustica::rhi::SamplerHandle            m_pointSampler;
    caustica::rhi::SamplerHandle            m_linearSampler;

    // caustica::rhi::BufferHandle             m_lightingConstants;                // same content as in control buffer

    caustica::rhi::BufferHandle             m_lightsBuffer;                     // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    caustica::rhi::BufferHandle             m_lightsExBuffer;                   // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    caustica::rhi::BufferHandle             m_scratchBuffer;                    // byte size: LLB_SCRATCH_BUFFER_SIZE
    caustica::rhi::BufferHandle             m_scratchList;                      // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    caustica::rhi::BufferHandle             m_historyRemapCurrentToPastBuffer;  // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    caustica::rhi::BufferHandle             m_historyRemapPastToCurrentBuffer;  // element count: CAUSTICA_LIGHTING_MAX_LIGHTS

    caustica::rhi::BufferHandle             m_controlBufferReadback;        // for showing debug info
    int                             m_framesFromLastReadbackCopy;   // the number of frames that passed since 
    LightingControlData             m_lastReadback;

    caustica::rhi::BufferHandle             m_lightWeights;                 // element count: 2 * CAUSTICA_LIGHTING_WEIGHTS_COUNT_HALF
    caustica::rhi::BufferHandle             m_perLightProxyCounters;        // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    caustica::rhi::BufferHandle             m_lightSamplingProxies;         // element count: CAUSTICA_LIGHTING_MAX_SAMPLING_PROXIES  <- this is the output of the GPUSort and is only used to sort the above 2 arrays

    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackTotalWeight;
    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackCandidates;
    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackTotalWeightScratch;
    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackCandidatesScratch;
    bool                            m_NEE_AT_FeedbackBufferFilled;

    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackTotalWeightBlended;
    caustica::rhi::TextureHandle            m_NEE_AT_FeedbackCandidatesBlended;
    caustica::rhi::BufferHandle             m_NEE_AT_LocalSamplingBuffer;

    caustica::rhi::TextureHandle            m_envLightLookupMap;            // used for looking up environment lights by direction for full MIS

    caustica::rhi::TextureHandle            m_NEE_AT_HistoryDepth;

    std::vector<PolymorphicLightInfo>   m_scratchLightBuffer;                           // these are for scene lights filled in on CPU side
    std::vector<PolymorphicLightInfoEx> m_scratchLightExBuffer;                         // these are for scene lights filled in on CPU side
    std::vector<uint>                   m_scratchLightHistoryRemapCurrentToPastBuffer;  // these are for scene lights filled in on CPU side
    std::vector<uint>                   m_scratchLightHistoryRemapPastToCurrentBuffer;  // these are for scene lights filled in on CPU side
    std::shared_ptr<std::vector<struct EmissiveTrianglesProcTask>>  m_scratchTaskBuffer;
    uint                                m_historicTotalLightCount;

    // NOTE: there's no mechanism to erase stale historic indices; it would be ideal to double-buffer both of these and each frame populate the new one afresh, while clearing the old one; that way we would never have leftover historic entries and reduce chance of hash collisions
    std::unordered_map<size_t, uint32_t> m_historyRemapEmissiveLightBlockOffsets;
    std::unordered_map<size_t, uint32_t> m_historyRemapAnalyticLightIndices;
    // Same-frame entity → analytic light index (for mesh analytic-light proxy resolution).
    std::unordered_map<uint32_t, uint32_t> m_currentFrameAnalyticLightIndex;

    float2                          m_localJitterF                      = {0, 0};
    uint2                           m_localJitter                       = {0, 0};
    uint2                           m_prevLocalJitter                   = {0, 0};
    uint                            m_updateCounter                     = 0;

    int                             m_localSamplingBufferWidth          = 0;
    int                             m_localSamplingBufferHeight         = 0;
    const int                       m_localSamplingBufferDepth          = CAUSTICA_LIGHTING_LOCAL_PROXY_COUNT;

    // various buffers are ping-ponged where current and history swap places; this bool is inverted at every update()
    bool                            m_ping                              = false;

    bool                            m_dbgDebugDrawLights                = false;
    bool                            m_dbgDebugDrawTileLightConnections  = false;
    bool                            m_dbgFreezeUpdates                  = false;
    
    LightingDebugViewType           m_dbgDebugDrawType                  = LightingDebugViewType::Disabled;
    bool                            m_dbgDebugDisableJitter             = false;
    bool                            m_dbgDebugDisableLastFrameFeedback  = false;

    float                           m_advSetting_ScreenSpaceVsWorldSpaceThreshold = 0.3f;
    bool                            m_advSetting_SampleBakedEnvironment = true;

    bool                            m_deviceHas32ThreadWaves            = false;

    bool                            m_importanceBoost_IntensityDelta        = true;
    float                           m_importanceBoost_IntensityDeltaMul     = 64.0f;   // seems like a good balance but it's just a rough guess
    bool                            m_importanceBoost_Frustum               = true;
    float                           m_importanceBoost_FrustumMul            = 8.0f;
    float                           m_importanceBoost_FrustumFadeDistance   = 5.0f;
    float                           m_importanceBoost_FrustumAngleExpand    = 5.0f;    // in degrees - TODO: not implemented
    bool                            m_importanceBoost_PreFilter             = true;

    float                           m_advSetting_reservoirHistoryDropoff    = 0.005f;

    float                           m_depthDisocclusionThreshold            = 1.5f;

    int64_t                         m_lastFrameIndex                        = -1;   // updated in UpdateFrame, no longer valid after
    
    bool                            m_dbgFreezeFrustumUpdates           = false;
    float4                          m_dbgFrozenFrustum[6];

    uint64_t                        m_allocatedVRAM                     = 0;

    //static const uint               s_safeMaxLights                     = CAUSTICA_LIGHTING_MAX_LIGHTS * 19 / 20;  // 95%  - there's a bug when more than 99.9% allocated that needs catching
    bool                            m_noOverflow                        = true;

    int                             m_verifyBeginHasMatchingEnd         = 0;
};
