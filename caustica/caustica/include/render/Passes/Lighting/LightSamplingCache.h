#pragma once

#include <render/Core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>
#include <vector>

#include <math/math.h>

#define NEEAT_BAKER_ONLY 1
#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>

#include <render/Core/ComputePass.h>

#include <shaders/SubInstanceData.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>

#include <filesystem>

using namespace caustica::math;

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render { class RenderDevice; }
    struct TextureData;
}

class ShaderDebug;
#include <scene/Scene.h>

// this is a fallback for when the engine light source can't be modified to add tracking (otherwise, see 'struct LightSamplerLink')
// #define HASH_LOOKUP_BASED_HISTORIC_LIGHT_SOURCE_MATCHING

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
    LightSamplingCache(nvrhi::IDevice* device);
    ~LightSamplingCache();

    // Reset scene related stuff
    void                            SceneReloaded();

    void                            CreateRenderPasses(std::shared_ptr<caustica::ShaderFactory> shaderFactory, nvrhi::IBindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, std::shared_ptr<ShaderDebug> shaderDebug, const uint2 renderResolution, const uint envMapProcessedResolution);

    // Main and only processing stage is split into UpdateBegin/UpdateEnd. These can be called one after the other as soon as screen space motion vectors are available.
    // The split is purely to facilitate any potential async compute.
    
    // UpdateBegin can happen in parallel with any other ray preparatory tracing work - anything from BVH building to laying down denoising layers. Emissive triangle emission must be accessible at this point.
    void                            UpdateBegin(nvrhi::ICommandList * commandList, caustica::BindingCache & bindingCache, const UpdateSettings & settings, double sceneTime, const std::shared_ptr<caustica::Scene> & scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer, std::vector<SubInstanceData> & subInstanceData, nvrhi::TextureHandle envMapProcessed);
    // UpdateEnd must happen BEFORE any light sampling (e.g. PT pass with NEE) but AFTER screen space motion vectors are available for reprojection.
    void                            UpdateEnd(nvrhi::ICommandList * commandList, caustica::BindingCache & bindingCache, const std::shared_ptr<caustica::Scene> & scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer, nvrhi::TextureHandle depthBuffer, nvrhi::TextureHandle motionVectors);

    nvrhi::BufferHandle             GetControlBuffer() const                    { return m_controlBuffer; }
    nvrhi::BufferHandle             GetLightBuffer() const                      { return m_lightsBuffer; }              // this is the list of lights
    nvrhi::BufferHandle             GetLightExBuffer() const                    { return m_lightsExBuffer; }            // this is the list of light (extended data)
    nvrhi::BufferHandle             GetLightProxyCounters() const               { return m_perLightProxyCounters; }     // these are counters of how many proxies each light has
    nvrhi::BufferHandle             GetLightSamplingProxies() const             { return m_lightSamplingProxies; }      // these are indices into the GetLightBuffer()

    nvrhi::TextureHandle            GetEnvLightLookupMap() const                { return m_envLightLookupMap; }

    nvrhi::BufferHandle             GetLocalSamplingBuffer() const              { return m_NEE_AT_LocalSamplingBuffer; }

    nvrhi::TextureHandle            GetFeedbackTotalWeight() const              { return m_NEE_AT_FeedbackTotalWeight; }
    nvrhi::TextureHandle            GetFeedbackCandidates() const               { return m_NEE_AT_FeedbackCandidates; }


    bool                            InfoGUI(float indent);
    bool                            DebugGUI(float indent);

    void                            SetGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros);

    bool                            TotalLightCountOverflow() const;

private:

    // output goes into m_scratchLightBuffer and 
    static bool                     CollectEnvmapLightPlaceholders(const UpdateSettings & settings, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPastBuffer, std::vector<uint> & outLightHistoryRemapPastToCurrent);
    bool                            CollectAnalyticLightsCPU(const UpdateSettings & settings, const std::shared_ptr<caustica::Scene> & scene, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPast, std::vector<uint> & outLightHistoryRemapPastToCurrent);
    bool                            CollectGaussianSplatEmissionProxies(const UpdateSettings & settings, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPast, std::vector<uint> & outLightHistoryRemapPastToCurrent);

    // this creates emissive triangle proc tasks and also does any required geometry instance (subInstance) processing such as analyt light proxies; has to happen AFTER CollectAnalyticLightsCPU
    bool                            ProcessEmissiveGeometry( const UpdateSettings & settings, const std::shared_ptr<caustica::Scene> & scene, std::vector<SubInstanceData> & subInstanceData, LightingControlData & ctrlBuff, std::vector<struct EmissiveTrianglesProcTask> & tasks );

    void                            FillBindings(nvrhi::BindingSetDesc& outBindingSetDesc, const std::shared_ptr<caustica::Scene> & scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer, nvrhi::TextureHandle depthBuffer, nvrhi::TextureHandle motionVectors, nvrhi::TextureHandle envMapProcessed);

    void                            UpdateFrustumConsts(LightSamplingCacheConstants & outConsts, const LightSamplingCache::UpdateSettings & settings);

    void                            UpdateLocalJitter();

private:
    nvrhi::DeviceHandle             m_device;
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
    
    nvrhi::BindingLayoutHandle      m_commonBindingLayout;

    UpdateSettings                    m_currentSettings;
    LightingControlData             m_currentCtrlBuff;              // NOTE: this does not include GPU-side changes, only the initial state set in Update

    nvrhi::BufferHandle             m_controlBuffer;

    nvrhi::SamplerHandle            m_pointSampler;
    nvrhi::SamplerHandle            m_linearSampler;

    // nvrhi::BufferHandle             m_lightingConstants;                // same content as in control buffer

    nvrhi::BufferHandle             m_lightsBuffer;                     // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    nvrhi::BufferHandle             m_lightsExBuffer;                   // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    nvrhi::BufferHandle             m_scratchBuffer;                    // byte size: LLB_SCRATCH_BUFFER_SIZE
    nvrhi::BufferHandle             m_scratchList;                      // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    nvrhi::BufferHandle             m_historyRemapCurrentToPastBuffer;  // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    nvrhi::BufferHandle             m_historyRemapPastToCurrentBuffer;  // element count: CAUSTICA_LIGHTING_MAX_LIGHTS

    nvrhi::BufferHandle             m_controlBufferReadback;        // for showing debug info
    int                             m_framesFromLastReadbackCopy;   // the number of frames that passed since 
    LightingControlData             m_lastReadback;

    nvrhi::BufferHandle             m_lightWeights;                 // element count: 2 * CAUSTICA_LIGHTING_WEIGHTS_COUNT_HALF
    nvrhi::BufferHandle             m_perLightProxyCounters;        // element count: CAUSTICA_LIGHTING_MAX_LIGHTS
    nvrhi::BufferHandle             m_lightSamplingProxies;         // element count: CAUSTICA_LIGHTING_MAX_SAMPLING_PROXIES  <- this is the output of the GPUSort and is only used to sort the above 2 arrays

    nvrhi::TextureHandle            m_NEE_AT_FeedbackTotalWeight;
    nvrhi::TextureHandle            m_NEE_AT_FeedbackCandidates;
    nvrhi::TextureHandle            m_NEE_AT_FeedbackTotalWeightScratch;
    nvrhi::TextureHandle            m_NEE_AT_FeedbackCandidatesScratch;
    bool                            m_NEE_AT_FeedbackBufferFilled;

    nvrhi::TextureHandle            m_NEE_AT_FeedbackTotalWeightBlended;
    nvrhi::TextureHandle            m_NEE_AT_FeedbackCandidatesBlended;
    nvrhi::BufferHandle             m_NEE_AT_LocalSamplingBuffer;

    nvrhi::TextureHandle            m_envLightLookupMap;            // used for looking up environment lights by direction for full MIS

    nvrhi::TextureHandle            m_NEE_AT_HistoryDepth;

    std::vector<PolymorphicLightInfo>   m_scratchLightBuffer;                           // these are for scene lights filled in on CPU side
    std::vector<PolymorphicLightInfoEx> m_scratchLightExBuffer;                         // these are for scene lights filled in on CPU side
    std::vector<uint>                   m_scratchLightHistoryRemapCurrentToPastBuffer;  // these are for scene lights filled in on CPU side
    std::vector<uint>                   m_scratchLightHistoryRemapPastToCurrentBuffer;  // these are for scene lights filled in on CPU side
    std::shared_ptr<std::vector<struct EmissiveTrianglesProcTask>>  m_scratchTaskBuffer;
    uint                                m_historicTotalLightCount;

    // NOTE: there's no mechanism to erase stale historic indices; it would be ideal to double-buffer both of these and each frame populate the new one afresh, while clearing the old one; that way we would never have leftover historic entries and reduce chance of hash collisions
#ifdef HASH_LOOKUP_BASED_HISTORIC_LIGHT_SOURCE_MATCHING
    std::unordered_map<size_t, uint32_t> m_historyRemapEmissiveLightBlockOffsets;
    std::unordered_map<size_t, uint32_t> m_historyRemapAnalyticLightIndices;
#endif

    float2                          m_localJitterF                      = {0, 0};
    uint2                           m_localJitter                       = {0, 0};
    uint2                           m_prevLocalJitter                   = {0, 0};
    uint                            m_updateCounter                     = 0;

    int                             m_localSamplingBufferWidth          = 0;
    int                             m_localSamplingBufferHeight         = 0;
    const int                       m_localSamplingBufferDepth          = CAUSTICA_LIGHTING_LOCAL_PROXY_COUNT;

    // various buffers are ping-ponged where current and history swap places; this bool is inverted at every Update()
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
