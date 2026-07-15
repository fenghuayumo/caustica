#include <render/passes/lighting/LightSamplingCache.h>
#include <render/SceneGpuResources.h>

#include <assets/loader/ShaderFactory.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>

#include <core/scope.h>

#include <rhi/utils.h>

#include <imgui/imgui_renderer.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneLightAccess.h>

#include <cmath>

#include <shaders/render/lighting/LightSamplingCache.hlsl>

#include <render/passes/debug/ShaderDebug.h>

#include <render/gpuSort/GPUSort.h>
#include <shaders/PathTracer/Utils/NoiseAndSequences.hlsli>

#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>

#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>

using namespace caustica::math;
using namespace caustica;

LightSamplingCache::LightSamplingCache(nvrhi::IDevice* device)
    : m_device(device)
{
    sceneReloaded();

#if 0 // Switch to this when nvrhi::Feature::WaveLaneCountMinMax lands
    nvrhi::WaveLaneCountMinMaxFeatureInfo waveLaneCountMinMaxFeatureInfo;
    if (m_device->queryFeatureSupport(nvrhi::Feature::WaveLaneCountMinMax, (void*)&waveLaneCountMinMaxFeatureInfo, sizeof(waveLaneCountMinMaxFeatureInfo)))
    {
        m_deviceHas32ThreadWaves = (waveLaneCountMinMaxFeatureInfo.minWaveLaneCount == 32) && (waveLaneCountMinMaxFeatureInfo.maxWaveLaneCount == 32);
    }
#elif CAUSTICA_WITH_DX12
    // Native DX12 version to query lane counts
    if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        ID3D12Device* d3dDevice = (ID3D12Device*)m_device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
        if (d3dDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)) == S_OK)
        {
            m_deviceHas32ThreadWaves = (options1.WaveLaneCountMin == 32) && (options1.WaveLaneCountMax == 32);
        }
    }
#endif
}

void LightSamplingCache::sceneReloaded() 
{ 
    m_NEE_AT_FeedbackBufferFilled = false;
    m_framesFromLastReadbackCopy = -1; 
    memset( &m_lastReadback, 0, sizeof(m_lastReadback) ); 

    // clear history
    m_historyRemapAnalyticLightIndices.clear();
    m_historyRemapEmissiveLightBlockOffsets.clear();
    m_currentFrameAnalyticLightIndex.clear();
    m_historicTotalLightCount = 0;
    m_lastFrameIndex = -1;
    memset(&m_currentCtrlBuff, 0, sizeof(m_currentCtrlBuff));
    memset(&m_currentSettings, 0, sizeof(m_currentSettings));
}

LightSamplingCache::~LightSamplingCache()
{
}

void LightSamplingCache::createRenderPasses(std::shared_ptr<caustica::ShaderFactory> shaderFactory, nvrhi::IBindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, std::shared_ptr<ShaderDebug> shaderDebug, const uint2 renderResolution, const uint envMapProcessedResolution)
{
    m_renderDevice = &renderDevice;
    m_shaderDebug = shaderDebug;

    std::vector<caustica::ShaderMacro> shaderMacros;
    //shaderMacros.push_back(caustica::ShaderMacro({              "BLEND_DEBUG_BUFFER", "1" }));

    const char * shaderFile = "caustica/shaders/render/lighting/LightSamplingCache.hlsl";
        
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),      // u_controlBuffer
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),      // u_lightsBuffer
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),      // u_lightsExBuffer
            nvrhi::BindingLayoutItem::RawBuffer_UAV(3),             // u_scratchBuffer
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(4),           // u_scratchList
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(5),           // u_lightWeights 
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(6),           // u_historyRemapCurrentToPast
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(7),           // u_historyRemapPastToCurrent
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(8),           // u_perLightProxyCounters
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(9),           // u_lightSamplingProxies
            nvrhi::BindingLayoutItem::Texture_UAV(10),              // u_envLightLookupMap
            //nvrhi::BindingLayoutItem::TypedBuffer_UAV(11),
            nvrhi::BindingLayoutItem::Texture_UAV(11),              // u_feedbackTotalWeight
            nvrhi::BindingLayoutItem::Texture_UAV(12),              // u_feedbackCandidates
            nvrhi::BindingLayoutItem::Texture_UAV(13),              // u_feedbackTotalWeightScratch
            nvrhi::BindingLayoutItem::Texture_UAV(14),              // u_feedbackCandidatesScratch
            nvrhi::BindingLayoutItem::Texture_UAV(15),              // u_feedbackTotalWeightBlended
            nvrhi::BindingLayoutItem::Texture_UAV(16),              // u_feedbackCandidatesBlended
            nvrhi::BindingLayoutItem::Texture_UAV(17),              // u_historyDepth
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(18),          // u_localSamplingBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(10),              // t_depthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(11),              // t_motionVectors
            nvrhi::BindingLayoutItem::Texture_SRV(12),              // t_envmapImportanceMap
            nvrhi::BindingLayoutItem::Sampler(0),                   // point sampler
            nvrhi::BindingLayoutItem::Sampler(1),                   // linear sampler
            nvrhi::BindingLayoutItem::Sampler(2),                   // s_MaterialSampler
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),      // StructuredBuffer<SubInstanceData> t_SubInstanceData
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),      // StructuredBuffer<InstanceData> t_InstanceData          
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),      // StructuredBuffer<GeometryData> t_GeometryData          
            //nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),      // geometry debug buffer not needed here?
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),      // StructuredBuffer<PTMaterialData> t_PTMaterialData
            nvrhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
            nvrhi::BindingLayoutItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX),
        };
        m_commonBindingLayout = m_device->createBindingLayout(layoutDesc);
    }

    nvrhi::ComputePipelineDesc pipelineDesc;

    // These need to know about the scene
    pipelineDesc.bindingLayouts = { m_commonBindingLayout, bindlessLayout };
    m_bakeEmissiveTriangles     .init(m_device, *shaderFactory, shaderFile, "BakeEmissiveTriangles",      shaderMacros, pipelineDesc.bindingLayouts);
    
    // these don't need to know anything about the scene
    pipelineDesc.bindingLayouts = { m_commonBindingLayout };

    m_resetPastToCurrentHistory.init(m_device, *shaderFactory, shaderFile, "ResetPastToCurrentHistory",   shaderMacros, pipelineDesc.bindingLayouts);

    m_envLightsBackupPast       .init(m_device, *shaderFactory, shaderFile, "EnvLightsBackupPast"      ,   shaderMacros, pipelineDesc.bindingLayouts);
    m_envLightsSubdivideBase    .init(m_device, *shaderFactory, shaderFile, "EnvLightsSubdivideBase"   ,   shaderMacros, pipelineDesc.bindingLayouts);
    m_envLightsSubdivideBoost   .init(m_device, *shaderFactory, shaderFile, "EnvLightsSubdivideBoost"  ,   shaderMacros, pipelineDesc.bindingLayouts);
    m_envLightsFillLookupMap    .init(m_device, *shaderFactory, shaderFile, "EnvLightsFillLookupMap"   ,   shaderMacros, pipelineDesc.bindingLayouts);
    m_envLightsMapPastToCurrent .init(m_device, *shaderFactory, shaderFile, "EnvLightsMapPastToCurrent",   shaderMacros, pipelineDesc.bindingLayouts);

    m_clearFeedbackHistory      .init(m_device, *shaderFactory, shaderFile, "ClearFeedbackHistory",        shaderMacros, pipelineDesc.bindingLayouts);

    m_processFeedbackHistoryP0      .init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryP0"        , shaderMacros, pipelineDesc.bindingLayouts);
    m_processFeedbackHistoryP1a     .init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryP1a"       , shaderMacros, pipelineDesc.bindingLayouts);
    m_processFeedbackHistoryP1b     .init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryP1b"       , shaderMacros, pipelineDesc.bindingLayouts);
    m_processFeedbackHistoryP2      .init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryP2"        , shaderMacros, pipelineDesc.bindingLayouts);
    m_processFeedbackHistoryP3      .init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryP3"        , shaderMacros, pipelineDesc.bindingLayouts);
    m_processFeedbackHistoryDebugViz.init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryDebugViz"  , shaderMacros, pipelineDesc.bindingLayouts);

    m_processFeedbackHistoryPreFilter.init(m_device, *shaderFactory, shaderFile, "ProcessFeedbackHistoryPreFilter", shaderMacros, pipelineDesc.bindingLayouts);

    m_resetLightProxyCounters       .init(m_device, *shaderFactory, shaderFile, "ResetLightProxyCounters"         , shaderMacros, pipelineDesc.bindingLayouts);
    m_computeWeights                .init(m_device, *shaderFactory, shaderFile, "ComputeWeights"                  , shaderMacros, pipelineDesc.bindingLayouts);
    m_computeProxyCounts            .init(m_device, *shaderFactory, shaderFile, "ComputeProxyCounts"              , shaderMacros, pipelineDesc.bindingLayouts);
    m_computeProxyBaselineOffsets   .init(m_device, *shaderFactory, shaderFile, "ComputeProxyBaselineOffsets"     , shaderMacros, pipelineDesc.bindingLayouts);
    m_createProxyJobs               .init(m_device, *shaderFactory, shaderFile, "CreateProxyJobs"                 , shaderMacros, pipelineDesc.bindingLayouts);
    m_executeProxyJobs              .init(m_device, *shaderFactory, shaderFile, "ExecuteProxyJobs"                , shaderMacros, pipelineDesc.bindingLayouts);
    m_debugDrawLights               .init(m_device, *shaderFactory, shaderFile, "DebugDrawLights"                 , shaderMacros, pipelineDesc.bindingLayouts);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setBorderColor(nvrhi::Color(0.f));
    samplerDesc.setAllFilters(true);
    samplerDesc.setMipFilter(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_linearSampler = m_device->createSampler(samplerDesc);

    samplerDesc.setAllFilters(false);
    m_pointSampler = m_device->createSampler(samplerDesc);

    // destroy resources before creating to avoid lifetimes of old and new overlapping (even with itself, due to assignment operator) - avoids fragmentation and peaks
    m_controlBuffer = m_lightsBuffer = m_lightsExBuffer = m_historyRemapCurrentToPastBuffer = m_historyRemapPastToCurrentBuffer = m_scratchBuffer = m_lightWeights = m_perLightProxyCounters = m_scratchList = m_lightSamplingProxies = nullptr;
    //m_lightingConstants = nullptr;
    m_device->waitForIdle();    // make sure readback buffer is no longer used by the GPU
    m_controlBufferReadback = nullptr;

    if (m_envLightLookupMap != nullptr && m_envLightLookupMap->getDesc().width != envMapProcessedResolution)
        m_envLightLookupMap = nullptr;

    assert(renderResolution.x > 0 && renderResolution.y > 0);
    if (m_NEE_AT_FeedbackTotalWeight == nullptr || m_NEE_AT_FeedbackTotalWeight->getDesc().width != renderResolution.x || m_NEE_AT_FeedbackTotalWeight->getDesc().height != renderResolution.y)
    {
        if (m_NEE_AT_FeedbackTotalWeight)
            m_device->waitForIdle();    // make sure none of the buffers are used by the GPU

        // destroy before creating to avoid lifetimes of old and new overlapping (even with itself, due to assignment operator) - avoids fragmentation and peaks
        m_NEE_AT_FeedbackTotalWeight = nullptr;
        m_NEE_AT_FeedbackCandidates = nullptr;
        m_NEE_AT_FeedbackTotalWeightScratch = nullptr;
        m_NEE_AT_FeedbackCandidatesScratch = nullptr;
        m_NEE_AT_FeedbackTotalWeightBlended = nullptr;
        m_NEE_AT_FeedbackCandidatesBlended = nullptr;

        m_NEE_AT_LocalSamplingBuffer = nullptr;
        m_NEE_AT_HistoryDepth = nullptr;
    }

    m_device->waitForIdle();    // make sure everything is deallocated and garbage collected

    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveUAVs = false;

        // Main control buffer holding all constants and build metadata
        bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.byteSize = sizeof(LightingControlData) * 1;
        bufferDesc.structStride = sizeof(LightingControlData);
        bufferDesc.debugName = "LightingControlData";
        m_controlBuffer = m_device->createBuffer(bufferDesc);

        // Lights buffer
        bufferDesc.byteSize = sizeof(PolymorphicLightInfo) * CAUSTICA_LIGHTING_MAX_LIGHTS;
        bufferDesc.structStride = sizeof(PolymorphicLightInfo);
        bufferDesc.debugName = "LightsBuffer";
        m_lightsBuffer = m_device->createBuffer(bufferDesc);
        
        bufferDesc.byteSize = sizeof(PolymorphicLightInfoEx) * CAUSTICA_LIGHTING_MAX_LIGHTS;
        bufferDesc.structStride = sizeof(PolymorphicLightInfoEx);
        bufferDesc.debugName = "LightsExBuffer";
        m_lightsExBuffer = m_device->createBuffer(bufferDesc);

        // Emissive triangle processing tasks buffer
        bufferDesc.structStride = 0;
        bufferDesc.byteSize = LLB_SCRATCH_BUFFER_SIZE;
        bufferDesc.canHaveRawViews = true;
        bufferDesc.debugName = "LightsScratchBuffer";
        m_scratchBuffer = m_device->createBuffer(bufferDesc);
        // CPU side scratch storage for emissive light processing
        m_scratchTaskBuffer = std::make_shared<std::vector<struct EmissiveTrianglesProcTask>>();
        m_scratchTaskBuffer->reserve(LLB_MAX_PROC_TASKS);

        // Subsequent buffers are non-structured
        bufferDesc.structStride = 0;
        bufferDesc.canHaveTypedViews = true;
        bufferDesc.canHaveRawViews = false;
        
        bufferDesc.byteSize = 2 * sizeof(float) * CAUSTICA_LIGHTING_WEIGHTS_COUNT_HALF;
        bufferDesc.format = nvrhi::Format::R32_FLOAT;
        bufferDesc.debugName = "LightsWeights";
        m_lightWeights = m_device->createBuffer(bufferDesc);

        bufferDesc.format = nvrhi::Format::R32_UINT;
        bufferDesc.debugName = "HistoryRemapCurrentToPast";
        m_historyRemapCurrentToPastBuffer = m_device->createBuffer(bufferDesc);
        bufferDesc.debugName = "HistoryRemapPastToCurrent";
        m_historyRemapPastToCurrentBuffer = m_device->createBuffer(bufferDesc);
        bufferDesc.debugName = "PerLightProxyCounters";
        m_perLightProxyCounters = m_device->createBuffer(bufferDesc);
        bufferDesc.debugName = "ScratchList";
        assert( bufferDesc.byteSize / sizeof(uint) >= (CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT*2) );    // we need at least 2 times CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT for temporary envmap quads stuff
        m_scratchList = m_device->createBuffer(bufferDesc);
        bufferDesc.byteSize = sizeof(uint) * CAUSTICA_LIGHTING_MAX_SAMPLING_PROXIES;
        bufferDesc.debugName = "LightSamplingProxies";
        m_lightSamplingProxies = m_device->createBuffer(bufferDesc);

        // For debugging/UI
        bufferDesc.canHaveUAVs = false;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        bufferDesc.structStride = 0;
        bufferDesc.keepInitialState = false;
        bufferDesc.canHaveTypedViews = false;
        bufferDesc.initialState = nvrhi::ResourceStates::Unknown;
        bufferDesc.debugName = "LightingControlDataReadback";
        m_controlBufferReadback = m_device->createBuffer(bufferDesc);
        m_framesFromLastReadbackCopy = -1;
    }

    if (m_envLightLookupMap == nullptr)
    {
        nvrhi::TextureDesc texDesc;
        texDesc.width = envMapProcessedResolution;  texDesc.height = envMapProcessedResolution; texDesc.mipLevels = 1;
        texDesc.format = nvrhi::Format::R32_UINT; texDesc.isRenderTarget = true; texDesc.isUAV = true;
        texDesc.setInitialState(nvrhi::ResourceStates::UnorderedAccess); texDesc.keepInitialState = true;
        texDesc.debugName = "EnvLightLookupMap";

        m_envLightLookupMap = m_device->createTexture(texDesc);
    }

    assert(renderResolution.x > 0 && renderResolution.y > 0);
    if (m_NEE_AT_FeedbackTotalWeight == nullptr)
    {
        // feedback reservoirs
        nvrhi::TextureDesc desc;
        desc.width = renderResolution.x;
        desc.height = renderResolution.y;
        desc.isVirtual = false;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.isRenderTarget = false;
        desc.useClearValue = false;
        desc.clearValue = nvrhi::Color(0.f);
        desc.sampleCount = 1;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.keepInitialState = true;
        desc.isTypeless = false;
        desc.isUAV = true;
        desc.mipLevels = 1;
        desc.format = nvrhi::Format::R32_FLOAT;
        desc.debugName = "NEE_AT_HistoryDepth";
        m_NEE_AT_HistoryDepth = m_device->createTexture(desc);
        desc.debugName = "NEE_AT_FeedbackTotalWeight";
        m_NEE_AT_FeedbackTotalWeight = m_device->createTexture(desc);
        desc.debugName = "NEE_AT_FeedbackTotalWeightScratch";
        m_NEE_AT_FeedbackTotalWeightScratch = m_device->createTexture(desc);
        nvrhi::TextureDesc miniDesc = desc; miniDesc.width = div_ceil(desc.width, CAUSTICA_NEEAT_EARLY_FEEDBACK_TILE_SIZE); miniDesc.height = div_ceil(desc.height, CAUSTICA_NEEAT_EARLY_FEEDBACK_TILE_SIZE);
        desc.debugName = "NEE_AT_EarlyFeedbackTotalWeightScratch";
        m_NEE_AT_FeedbackTotalWeightBlended = m_device->createTexture(miniDesc);
        m_NEE_AT_FeedbackBufferFilled = false;
        desc.format = nvrhi::Format::R32_UINT;
        desc.debugName = "NEE_AT_FeedbackCandidates";
        m_NEE_AT_FeedbackCandidates = m_device->createTexture(desc);
        desc.debugName = "NEE_AT_FeedbackCandidatesScratch";
        m_NEE_AT_FeedbackCandidatesScratch = m_device->createTexture(desc);
        miniDesc = desc; miniDesc.width = div_ceil(desc.width, CAUSTICA_NEEAT_EARLY_FEEDBACK_TILE_SIZE); miniDesc.height = div_ceil(desc.height, CAUSTICA_NEEAT_EARLY_FEEDBACK_TILE_SIZE);
        desc.debugName = "NEE_AT_EarlyFeedbackCandidatesScratch";
        m_NEE_AT_FeedbackCandidatesBlended = m_device->createTexture(miniDesc);

        {
            m_localSamplingBufferWidth  = dm::div_ceil(renderResolution.x, CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE);
            m_localSamplingBufferHeight = dm::div_ceil(renderResolution.y, CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE);
            m_localSamplingBufferWidth  += 1;   // add border to accommodate for jitter offset for the local sampling buffers
            m_localSamplingBufferHeight += 1;   // add border to accommodate for jitter offset for the local sampling buffers
            // m_localSamplingBufferDepth          = CAUSTICA_LIGHTING_LOCAL_PROXY_COUNT
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bufferDesc.keepInitialState = true;
            bufferDesc.byteSize = sizeof(uint) * m_localSamplingBufferWidth * m_localSamplingBufferHeight * m_localSamplingBufferDepth;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = false;
            bufferDesc.format = nvrhi::Format::R32_UINT;
            bufferDesc.debugName = "NEE_AT_LocalSamplingBuffer";
            m_NEE_AT_LocalSamplingBuffer = m_device->createBuffer(bufferDesc);
        }


        assert(CAUSTICA_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE>=CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE && ((CAUSTICA_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE-CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE)%2==0));
    }

    m_allocatedVRAM = 0;
    m_allocatedVRAM += m_controlBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_lightsBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_lightsExBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_scratchBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_scratchList->getDesc().byteSize;
    m_allocatedVRAM += m_historyRemapCurrentToPastBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_historyRemapPastToCurrentBuffer->getDesc().byteSize;
    m_allocatedVRAM += m_lightWeights->getDesc().byteSize;
    m_allocatedVRAM += m_perLightProxyCounters->getDesc().byteSize;
    m_allocatedVRAM += m_lightSamplingProxies->getDesc().byteSize;
    m_allocatedVRAM += m_NEE_AT_LocalSamplingBuffer->getDesc().byteSize;
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackTotalWeight->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackCandidates->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackTotalWeightScratch->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackCandidatesScratch->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackTotalWeightBlended->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_FeedbackCandidatesBlended->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_envLightLookupMap->getDesc());
    m_allocatedVRAM += getEstimatedTextureSize(m_NEE_AT_HistoryDepth->getDesc());

    sceneReloaded();
}

// TODO: combine these

static inline uint floatToUInt(float _V, float _Scale)
{
    return (uint)floor(_V * _Scale + 0.5f);
}

static inline uint FLOAT3_to_R8G8B8_UNORM(float unpackedInputX, float unpackedInputY, float unpackedInputZ)
{
    return (floatToUInt(saturate(unpackedInputX), 0xFF) & 0xFF) |
        ((floatToUInt(saturate(unpackedInputY), 0xFF) & 0xFF) << 8) |
        ((floatToUInt(saturate(unpackedInputZ), 0xFF) & 0xFF) << 16);
}

static void packLightColor(const float3& color, PolymorphicLightInfo& lightInfo)
{
    float maxRadiance = std::max(color.x, std::max(color.y, color.z));

    if (maxRadiance <= 0.f)
        return;

    float logRadiance = (::log2f(maxRadiance) - kPolymorphicLightMinLog2Radiance) / (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance);
    logRadiance = saturate(logRadiance);
    uint32_t packedRadiance = std::min(uint32_t(ceilf(logRadiance * 65534.f)) + 1, 0xffffu);
    float unpackedRadiance = ::exp2f((float(packedRadiance - 1) / 65534.f) * (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) + kPolymorphicLightMinLog2Radiance);

    lightInfo.ColorTypeAndFlags |= FLOAT3_to_R8G8B8_UNORM(color.x / unpackedRadiance, color.y / unpackedRadiance, color.z / unpackedRadiance);
    lightInfo.LogRadiance |= packedRadiance;
    assert((lightInfo.LogRadiance & 0xFFFF0000)==0); 
}

// TODO: move this to Utils.hlsli and include from here and it should all work
static float2 OctWrap(float2 v)
{
    return float2((1.0f - abs(v.y)) * ((v.x >= 0.0) ? 1.0f : -1.0f),
        (1.0f - abs(v.x)) * ((v.y >= 0.0) ? 1.0f : -1.0f));
}

static float2 Encode_Oct(float3 n3)
{
    n3 /= (abs(n3.x) + abs(n3.y) + abs(n3.z));
    float2 n = n3.xy();
    n = n3.z >= 0.0 ? n : OctWrap(n);
    n = n * 0.5f + 0.5f;
    return n;
}

static uint NDirToOctUnorm32(float3 n)
{
    float2 p = Encode_Oct(n);
    p = saturate(p * 0.5f + 0.5f);
    return uint(p.x * 0xfffe) | (uint(p.y * 0xfffe) << 16);
}

// Modified from original, based on the method from the DX fallback layer sample
static uint16_t fp32ToFp16(float v)
{
    // Multiplying by 2^-112 causes exponents below -14 to denormalize
    static const union FU {
        uint ui;
        float f;
    } multiple = { 0x07800000 }; // 2**-112

    FU BiasedFloat;
    BiasedFloat.f = v * multiple.f;
    const uint u = BiasedFloat.ui;

    const uint sign = u & 0x80000000;
    uint body = u & 0x0fffffff;

    return (uint16_t)(sign >> 16 | body >> 13) & 0xFFFF;
}

static PolymorphicLightInfoFull ConvertLightProxy(
    const caustica::scene::LightRenderProxy& proxy)
{
    PolymorphicLightInfo polymorphic; memset(&polymorphic, 0, sizeof(polymorphic));
    PolymorphicLightInfoEx polymorphicEx; memset(&polymorphicEx, 0, sizeof(polymorphicEx));

    const float3 lightPos = float3(caustica::scene::getLightPosition(proxy.transform));
    const float3 lightDir = float3(normalize(caustica::scene::getLightDirection(proxy.transform)));

    switch (caustica::scene::getLightType(proxy))
    {
    case LightType_Spot:
    {
        const auto& spot = std::get<caustica::scene::SpotLightData>(proxy.data);
        if (spot.radius == 0.f)
        {
            assert(false); // not tested with radius == 0
            float3 flux = proxy.color * spot.intensity;
            polymorphic.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kPoint << kPolymorphicLightTypeShift | ((spot.outerAngle < 0) ? kPolymorphicLightShapingUseMinFalloff : 0);
            packLightColor(flux, polymorphic);
            polymorphic.Center = lightPos;
            polymorphic.Direction1 = NDirToOctUnorm32(lightDir);
            polymorphic.Direction2 = fp32ToFp16(dm::radians(abs(spot.outerAngle)));
            polymorphic.Direction2 |= fp32ToFp16(dm::radians(spot.innerAngle)) << 16;
        }
        else
        {
            float projectedArea = dm::PI_f * (spot.radius * spot.radius);
            float3 radiance = proxy.color * spot.intensity / projectedArea;
            float softness = saturate(1.f - spot.innerAngle / abs(spot.outerAngle));
            polymorphic.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift | ((spot.outerAngle < 0) ? kPolymorphicLightShapingUseMinFalloff : 0);
            polymorphic.ColorTypeAndFlags |= kPolymorphicLightShapingEnableBit;
            packLightColor(radiance, polymorphic);
            polymorphic.Center = lightPos;
            polymorphic.Scalars = fp32ToFp16(spot.radius);
            if (abs(spot.outerAngle) > 0)
            {
                polymorphic.ColorTypeAndFlags |= kPolymorphicLightShapingEnableBit;
                polymorphicEx.PrimaryAxis = NDirToOctUnorm32(lightDir);
                polymorphicEx.CosConeAngleAndSoftness = fp32ToFp16(cosf(dm::radians(abs(spot.outerAngle))));
                polymorphicEx.CosConeAngleAndSoftness |= fp32ToFp16(softness) << 16;
            }
            packLightColor(radiance, polymorphic);
        }
    } break;
    case LightType_Point:
    {
        const auto& point = std::get<caustica::scene::PointLightData>(proxy.data);
        if (point.radius == 0.f)
        {
            float3 flux = proxy.color * point.intensity;
            polymorphic.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kPoint << kPolymorphicLightTypeShift;
            packLightColor(flux, polymorphic);
            polymorphic.Center = lightPos;
            polymorphic.Direction2 = fp32ToFp16(dm::PI_f) | fp32ToFp16(0.0f) << 16;
        }
        else
        {
            float projectedArea = dm::PI_f * (point.radius * point.radius);
            float3 radiance = proxy.color * point.intensity / projectedArea;
            polymorphic.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift;
            packLightColor(radiance, polymorphic);
            polymorphic.Center = lightPos;
            polymorphic.Scalars = fp32ToFp16(point.radius);
        }
    } break;
    default: break;
    }

    return PolymorphicLightInfoFull::make(polymorphic, polymorphicEx);
}

static float3 TransformPoint(const float3& point, const float4x4& transform)
{
    const float4 transformed = float4(point, 1.0f) * transform;
    const float invW = std::abs(transformed.w) > 1e-6f ? 1.0f / transformed.w : 1.0f;
    return transformed.xyz() * invW;
}

static float TransformRadiusScale(const float4x4& transform)
{
    const float3 row0 = float3(transform.row0.x, transform.row0.y, transform.row0.z);
    const float3 row1 = float3(transform.row1.x, transform.row1.y, transform.row1.z);
    const float3 row2 = float3(transform.row2.x, transform.row2.y, transform.row2.z);
    return std::max(1e-4f, std::max(dm::length(row0), std::max(dm::length(row1), dm::length(row2))));
}

static PolymorphicLightInfoFull ConvertGaussianSplatEmissionProxy(
    const GaussianSplatEmissionProxy& proxy,
    const float4x4& objectToWorld,
    float emissionIntensity,
    uint32_t proxyIndex)
{
    PolymorphicLightInfo polymorphic; memset(&polymorphic, 0, sizeof(polymorphic));
    PolymorphicLightInfoEx polymorphicEx; memset(&polymorphicEx, 0, sizeof(polymorphicEx));

    const float3 radiance = float3(
        std::max(proxy.radiance.x * emissionIntensity, 0.0f),
        std::max(proxy.radiance.y * emissionIntensity, 0.0f),
        std::max(proxy.radiance.z * emissionIntensity, 0.0f));
    const float radius = std::max(proxy.radius * TransformRadiusScale(objectToWorld), 1e-4f);

    polymorphic.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift;
    packLightColor(radiance, polymorphic);
    polymorphic.Center = TransformPoint(proxy.center, objectToWorld);
    polymorphic.Scalars = fp32ToFp16(radius);
    polymorphicEx.UniqueID = Hash32Combine(0x3D650000u, proxyIndex);

    return PolymorphicLightInfoFull::make(polymorphic, polymorphicEx);
}

// #ifdef _DEBUG
// #pragma optimize("gt", on)
// #endif
bool LightSamplingCache::collectEnvmapLightPlaceholders(const UpdateSettings & settings, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPastBuffer, std::vector<uint> & outLightHistoryRemapPastToCurrent)
{
    ctrlBuff.EnvmapQuadNodeCount += CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT;
    ctrlBuff.TotalLightCount += CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT;
    assert( ctrlBuff.TotalLightCount < CAUSTICA_LIGHTING_MAX_LIGHTS );

    assert( outLightBuffer.size() == 0 );
    assert( outLightExBuffer.size() == 0 );

    // insert placeholder light info
    PolymorphicLightInfo dummy; memset(&dummy, 0, sizeof(dummy));
    PolymorphicLightInfoEx dummyEx; memset(&dummyEx, 0, sizeof(dummyEx));
    dummy.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kEnvironmentQuad << kPolymorphicLightTypeShift;   // no need to fill this, it will be completely overwritten
    outLightBuffer.insert( outLightBuffer.end(), CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, dummy );
    outLightExBuffer.insert( outLightExBuffer.end(), CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, dummyEx );

    outLightHistoryRemapCurrentToPastBuffer.insert(outLightHistoryRemapCurrentToPastBuffer.end(), CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, CAUSTICA_INVALID_LIGHT_INDEX);
    outLightHistoryRemapPastToCurrent.insert(outLightHistoryRemapPastToCurrent.end(), ctrlBuff.EnvmapQuadNodeCount, CAUSTICA_INVALID_LIGHT_INDEX);
    return ctrlBuff.TotalLightCount < CAUSTICA_LIGHTING_MAX_LIGHTS;
}

// #ifdef _DEBUG
// #pragma optimize("gt", on)
// #endif
bool LightSamplingCache::collectAnalyticLightsCPU(const UpdateSettings & settings, const std::shared_ptr<caustica::Scene> & scene, LightingControlData & ctrlBuff, std::vector<PolymorphicLightInfo> & outLightBuffer, std::vector<PolymorphicLightInfoEx> & outLightExBuffer, std::vector<uint> & outLightHistoryRemapCurrentToPastBuffer, std::vector<uint> & outLightHistoryRemapPastToCurrent)
{
    (void)settings;
    bool allGood = true;
    m_currentFrameAnalyticLightIndex.clear();

    for (const scene::LightRenderProxy& lightProxy : scene->getRenderData().lights)
    {
        if (outLightBuffer.size() >= CAUSTICA_LIGHTING_MAX_LIGHTS)
        {
            assert(false); // no more room for lights!
            break;
        }

        const int lightType = caustica::scene::getLightType(lightProxy);
        switch (lightType)
        {
        case LightType_Spot:
        case LightType_Point:
        {
            PolymorphicLightInfoFull lightPackedFull = ConvertLightProxy(lightProxy);
            outLightBuffer.push_back( lightPackedFull.Base );
            outLightExBuffer.push_back( lightPackedFull.Extended );

            const uint32_t entityId = uint32_t(lightProxy.entity);
            outLightExBuffer.back().UniqueID = Hash32Combine(entityId, 0u);

            size_t lightHash = size_t(entityId);
            uint historicIndex = CAUSTICA_INVALID_LIGHT_INDEX;
            auto entry = m_historyRemapAnalyticLightIndices.find(lightHash);
            if( entry != m_historyRemapAnalyticLightIndices.end() )
            {
                historicIndex = entry->second;
                entry->second = ctrlBuff.TotalLightCount;
            }
            else
                m_historyRemapAnalyticLightIndices.insert( std::make_pair(lightHash, ctrlBuff.TotalLightCount) );

            outLightHistoryRemapCurrentToPastBuffer.push_back(historicIndex);
            m_currentFrameAnalyticLightIndex[entityId] = ctrlBuff.TotalLightCount;

            ctrlBuff.AnalyticLightCount++;
            ctrlBuff.TotalLightCount++;
        } break;
        default: break;
        }
    }

    // use current-to-past to create past-to-current: 1st init past-to-current values to invalid; then fill them up for those we can find historic match
    uint startingLight = (uint)outLightHistoryRemapPastToCurrent.size(); assert( startingLight == CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT ); // we know we should have envmap placeholders set up before so do sanity check
    outLightHistoryRemapPastToCurrent.insert(outLightHistoryRemapPastToCurrent.end(), ctrlBuff.AnalyticLightCount, CAUSTICA_INVALID_LIGHT_INDEX);
    for( uint lightIndex = startingLight; lightIndex < outLightHistoryRemapCurrentToPastBuffer.size(); lightIndex++ )
    {
        uint historicIndex = outLightHistoryRemapCurrentToPastBuffer[lightIndex];
        if( historicIndex != CAUSTICA_INVALID_LIGHT_INDEX )
            outLightHistoryRemapPastToCurrent[historicIndex] = lightIndex;
    }

    assert(outLightBuffer.size() == outLightHistoryRemapCurrentToPastBuffer.size());
    return allGood;
};

bool LightSamplingCache::collectGaussianSplatEmissionProxies(
    const UpdateSettings& settings,
    LightingControlData& ctrlBuff,
    std::vector<PolymorphicLightInfo>& outLightBuffer,
    std::vector<PolymorphicLightInfoEx>& outLightExBuffer,
    std::vector<uint>& outLightHistoryRemapCurrentToPast,
    std::vector<uint>& outLightHistoryRemapPastToCurrent)
{
    if (settings.GaussianSplatEmissionProxies == nullptr || settings.GaussianSplatEmissionIntensity <= 0.0f)
        return true;

    bool allGood = true;
    const std::vector<GaussianSplatEmissionProxy>& proxies = *settings.GaussianSplatEmissionProxies;

    for (uint32_t proxyIndex = 0; proxyIndex < proxies.size(); ++proxyIndex)
    {
        if (outLightBuffer.size() >= CAUSTICA_LIGHTING_MAX_LIGHTS)
        {
            allGood = false;
            break;
        }

        const PolymorphicLightInfoFull lightPackedFull = ConvertGaussianSplatEmissionProxy(
            proxies[proxyIndex],
            settings.GaussianSplatEmissionObjectToWorld,
            settings.GaussianSplatEmissionIntensity,
            proxyIndex);

        outLightBuffer.push_back(lightPackedFull.Base);
        outLightExBuffer.push_back(lightPackedFull.Extended);
        outLightHistoryRemapCurrentToPast.push_back(CAUSTICA_INVALID_LIGHT_INDEX);
        outLightHistoryRemapPastToCurrent.push_back(CAUSTICA_INVALID_LIGHT_INDEX);

        ctrlBuff.AnalyticLightCount++;
        ctrlBuff.TotalLightCount++;
    }

    return allGood;
}

// #ifdef _DEBUG
// #pragma optimize("gt", on)
// #endif
bool LightSamplingCache::processEmissiveGeometry( const UpdateSettings & settings, const std::shared_ptr<caustica::Scene> & scene, std::vector<SubInstanceData> & subInstanceData, LightingControlData & ctrlBuff, std::vector<struct EmissiveTrianglesProcTask> & tasks )
{
    (void)settings;
    bool allGood = true;

    tasks.clear();

    assert( ctrlBuff.TotalLightCount == (ctrlBuff.AnalyticLightCount+ctrlBuff.EnvmapQuadNodeCount) );

    // Same dense geometryInstance prefix as TLAS / MaterialGpuCache / hit-group fill.
    // Stale proxy.geometryInstanceIndex after runtime import must not skip emissives.
    size_t compactedGeometryInstanceIndex = 0;
    for (const scene::MeshInstanceRenderProxy& meshProxy : scene->getRenderData().meshInstances)
    {
        const auto& mesh = meshProxy.meshShared;
        if (!mesh)
            continue;

        const uint32_t firstGeometryInstanceIndex =
            static_cast<uint32_t>(compactedGeometryInstanceIndex);
        compactedGeometryInstanceIndex += mesh->geometries.size();

        for (size_t geometryIndex = 0; geometryIndex < mesh->geometries.size(); ++geometryIndex)
        {
            const auto& geometry = mesh->geometries[geometryIndex];
            const size_t subInstanceIndex = size_t(firstGeometryInstanceIndex) + geometryIndex;
            if (!geometry || subInstanceIndex >= subInstanceData.size())
            {
                assert(false && "Sub-instance data is out of sync with scene geometry instances");
                continue;
            }

            size_t instanceHash = 0;
            nvrhi::hash_combine(instanceHash, static_cast<uint32_t>(meshProxy.entity));
            nvrhi::hash_combine(instanceHash, geometryIndex);

            std::shared_ptr<PTMaterial> materialPTPtr = PTMaterial::safeCast(geometry->material);
            if (!materialPTPtr)
                continue;
            PTMaterial & materialPT = *materialPTPtr;

            // Analytic-light proxy binding after hitting this mesh (not emissive geometry).
            uint analyticProxyLightIndex = CAUSTICA_INVALID_LIGHT_INDEX;
            if (materialPT.enableAsAnalyticLightProxy)
            {
                auto resolveAnalyticIndex = [&](ecs::Entity lightEntity) -> uint {
                    if (!ecs::isValid(lightEntity))
                        return CAUSTICA_INVALID_LIGHT_INDEX;
                    const auto it = m_currentFrameAnalyticLightIndex.find(uint32_t(lightEntity));
                    if (it == m_currentFrameAnalyticLightIndex.end())
                        return CAUSTICA_INVALID_LIGHT_INDEX;
                    return it->second;
                };

                if (ecs::isValid(meshProxy.parentLightEntity))
                {
                    if (const scene::LightRenderProxy* parentLight =
                            scene->getRenderData().findLight(meshProxy.parentLightEntity))
                    {
                        const int lType = caustica::scene::getLightType(*parentLight);
                        if (lType == LightType_Spot || lType == LightType_Point)
                            analyticProxyLightIndex = resolveAnalyticIndex(meshProxy.parentLightEntity);
                    }
                }

                if (analyticProxyLightIndex == CAUSTICA_INVALID_LIGHT_INDEX)
                    analyticProxyLightIndex = resolveAnalyticIndex(meshProxy.proxiedAnalyticLight);
            }

            // now set the convertedLightIndex into subInstanceData - if CAUSTICA_INVALID_LIGHT_INDEX that's fine, nothing happens
            subInstanceData[subInstanceIndex].AnalyticProxyLightIndex = analyticProxyLightIndex;

            bool overflow = (ctrlBuff.TotalLightCount + (geometry->numIndices / 3) >= CAUSTICA_LIGHTING_MAX_LIGHTS);
            allGood &= !overflow;

            if (!materialPT.isEmissive() || materialPT.skipRender || overflow)
            {
                // remove the info about this instance, just in case it was emissive and now it's not
                m_historyRemapEmissiveLightBlockOffsets.erase(instanceHash);
                subInstanceData[subInstanceIndex].EmissiveLightMappingOffset = 0xFFFFFFFF;
                continue;
            }

            uint historicBufferOffset = CAUSTICA_INVALID_LIGHT_INDEX;
            auto entry = m_historyRemapEmissiveLightBlockOffsets.find(instanceHash);
            if (entry != m_historyRemapEmissiveLightBlockOffsets.end())
            {
                historicBufferOffset = entry->second;
                entry->second = ctrlBuff.TotalLightCount; // update with the new index for next search; lights should be unique
                subInstanceData[subInstanceIndex].EmissiveLightMappingOffset = 0xFFFFFFFF;
            }
            else
                m_historyRemapEmissiveLightBlockOffsets.insert(std::make_pair(instanceHash, ctrlBuff.TotalLightCount));

            subInstanceData[subInstanceIndex].EmissiveLightMappingOffset = ctrlBuff.TotalLightCount;

            assert(geometryIndex < 0xfff);

            int triangleFrom = 0;
            int remainingTriangles = geometry->numIndices / 3;
            assert( geometry->numIndices % 3 == 0 );

            while( remainingTriangles > 0 )
            {
                EmissiveTrianglesProcTask task;

                task.InstanceIndex      = meshProxy.instanceIndex;
                task.GeometryIndex      = (uint)geometryIndex;
                task.TriangleIndexFrom  = triangleFrom;
                int taskTriangleCount   = std::min( remainingTriangles, LLB_MAX_TRIANGLES_PER_TASK );
                task.TriangleIndexTo    = task.TriangleIndexFrom + taskTriangleCount;
                triangleFrom += taskTriangleCount;
                task.DestinationBufferOffset = ctrlBuff.TotalLightCount;
                task.HistoricBufferOffset = historicBufferOffset;

                tasks.push_back(task);

                if( tasks.size() >= LLB_MAX_PROC_TASKS )
                {
                    assert( false && "Emissive triangle task buffer too small" );
                    return false;
                }

                remainingTriangles -= taskTriangleCount;
                ctrlBuff.TotalLightCount += taskTriangleCount;
                ctrlBuff.TriangleLightCount += taskTriangleCount;

                if( historicBufferOffset != CAUSTICA_INVALID_LIGHT_INDEX )
                    historicBufferOffset += taskTriangleCount;
            }
        }
    }

    return allGood;
}

bool LightSamplingCache::totalLightCountOverflow() const
{
    return !m_noOverflow;
}

void LightSamplingCache::fillBindings(nvrhi::BindingSetDesc& outBindingSetDesc, const std::shared_ptr<caustica::Scene>& scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer,
nvrhi::TextureHandle depthBuffer, nvrhi::TextureHandle motionVectors, nvrhi::TextureHandle envMapProcessed)
{
    if( depthBuffer == nullptr )
        depthBuffer = ((nvrhi::TextureHandle)m_renderDevice->builtins().blackTexture().Get());
    if (motionVectors == nullptr)
        motionVectors = ((nvrhi::TextureHandle)m_renderDevice->builtins().blackTexture().Get());
    nvrhi::TextureHandle envMapRadianceAndImportanceMap = envMapProcessed;
    if (envMapRadianceAndImportanceMap == nullptr || !m_currentSettings.EnvMapParams.Enabled )
        envMapRadianceAndImportanceMap = ((nvrhi::TextureHandle)m_renderDevice->builtins().blackTexture().Get());

    outBindingSetDesc.bindings = {
            //nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
            //nvrhi::BindingSetItem::PushConstants(1, sizeof(SampleMiniConstants)),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_controlBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_lightsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_lightsExBuffer),
            nvrhi::BindingSetItem::RawBuffer_UAV(3, m_scratchBuffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(4, m_scratchList),
            nvrhi::BindingSetItem::TypedBuffer_UAV(5, m_lightWeights),
            nvrhi::BindingSetItem::TypedBuffer_UAV(6, m_historyRemapCurrentToPastBuffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(7, m_historyRemapPastToCurrentBuffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(8, m_perLightProxyCounters),
            nvrhi::BindingSetItem::TypedBuffer_UAV(9, m_lightSamplingProxies),
            nvrhi::BindingSetItem::Texture_UAV(10, m_envLightLookupMap),
            //nvrhi::BindingSetItem::TypedBuffer_UAV(11, ),
            nvrhi::BindingSetItem::Texture_UAV(11, m_NEE_AT_FeedbackTotalWeight ),
            nvrhi::BindingSetItem::Texture_UAV(12, m_NEE_AT_FeedbackCandidates ),
            nvrhi::BindingSetItem::Texture_UAV(13, m_NEE_AT_FeedbackTotalWeightScratch ),
            nvrhi::BindingSetItem::Texture_UAV(14, m_NEE_AT_FeedbackCandidatesScratch ),
            nvrhi::BindingSetItem::Texture_UAV(15, m_NEE_AT_FeedbackTotalWeightBlended ),
            nvrhi::BindingSetItem::Texture_UAV(16, m_NEE_AT_FeedbackCandidatesBlended ),
            nvrhi::BindingSetItem::Texture_UAV(17, m_NEE_AT_HistoryDepth ),
            nvrhi::BindingSetItem::TypedBuffer_UAV(18, m_NEE_AT_LocalSamplingBuffer),
            nvrhi::BindingSetItem::Texture_SRV(10, depthBuffer), //((nvrhi::TextureHandle)m_NEE_AT_FeedbackBuffer.Get())),
            nvrhi::BindingSetItem::Texture_SRV(11, motionVectors),
            nvrhi::BindingSetItem::Texture_SRV(12, envMapRadianceAndImportanceMap),
            nvrhi::BindingSetItem::Sampler(0, m_pointSampler),
            nvrhi::BindingSetItem::Sampler(1, m_linearSampler),
            nvrhi::BindingSetItem::Sampler(2, m_renderDevice->samplers().anisotropicWrap()),    // s_MaterialSampler
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, subInstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, scene->getGpuResources().instanceBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, scene->getGpuResources().geometryBuffer),
            //nvrhi::BindingSetItem::StructuredBuffer_SRV(4, opacityMicromapBuilder->getGeometryDebugBuffer()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(5, materialGpuCache->getMaterialDataBuffer()),
            nvrhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
            nvrhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->getDebugVizTexture()),
    };
}

void LightSamplingCache::updateFrustumConsts(LightSamplingCacheConstants & outConsts, const LightSamplingCache::UpdateSettings & settings)
{
    float4 frustPlanes[6];

    auto vp = [&settings](int row, int col) { return settings.ViewProjMatrix.col(col)[row]; };
    // Left clipping plane
    frustPlanes[0] = float4( vp(0, 3) + vp(0, 0), vp(1, 3) + vp(1, 0), vp(2, 3) + vp(2, 0), -(vp(3, 3) + vp(3, 0)));
    // Right clipping plane
    frustPlanes[1] = float4( vp(0, 3) - vp(0, 0), vp(1, 3) - vp(1, 0), vp(2, 3) - vp(2, 0), -(vp(3, 3) - vp(3, 0)));
    // Top clipping plane
    frustPlanes[2] = float4( vp(0, 3) - vp(0, 1), vp(1, 3) - vp(1, 1), vp(2, 3) - vp(2, 1), -(vp(3, 3) - vp(3, 1)));
    // Bottom clipping plane
    frustPlanes[3] = float4( vp(0, 3) + vp(0, 1), vp(1, 3) + vp(1, 1), vp(2, 3) + vp(2, 1), -(vp(3, 3) + vp(3, 1)));
    // Near clipping plane
    frustPlanes[4] = float4( vp(0, 3) - vp(0, 2), vp(1, 3) - vp(1, 2), vp(2, 3) - vp(2, 2), -(vp(3, 3) - vp(3, 2)));

    auto normalizePlane = [ ](const float4& plane)
    {
        float lengthSq = dot(plane.xyz(), plane.xyz());
        float scale = (lengthSq > 0.f ? (1.0f / sqrtf(lengthSq)) : 0);
        return plane * scale;
    };

    // Normalize the plane equations
    for (int i = 0; i < 5; i++)
        frustPlanes[i] = normalizePlane(frustPlanes[i]);

    // compute far plane with inverted near plane pushed away by DISTANT_LIGHT_DISTANCE
    frustPlanes[5] = dm::float4(-frustPlanes[4].xyz(), -frustPlanes[4].w - DISTANT_LIGHT_DISTANCE);

    // backup for debugging and sanity check and write to const buffer
    for (int i = 0; i < 6; i++)
    {
        float dist = dm::dot(frustPlanes[i].xyz(), settings.CameraPosition + settings.CameraDirection * float(DISTANT_LIGHT_DISTANCE * 0.001f) ) - frustPlanes[i].w;
        assert( dist > 0 );
        if (m_dbgFreezeFrustumUpdates)
            frustPlanes[i] = m_dbgFrozenFrustum[i];
        else
            m_dbgFrozenFrustum[i] = frustPlanes[i];
        outConsts.FrustumPlanes[i] = frustPlanes[i];
    }
    outConsts.DebugDrawFrustum  = m_dbgFreezeFrustumUpdates;

    auto getCorner = [&](int index) 
    {
        bool bone = (index & 1) != 0;
        bool btwo = (index & 2) != 0;
        const float4 & a = (bone == btwo) ? frustPlanes[1] : frustPlanes[0];
        const float4 & b = (index & 2) ? frustPlanes[3] : frustPlanes[2];
        const float4 & c = (index & 4) ? frustPlanes[5] : frustPlanes[4];

        float3x3 m = float3x3(a.xyz(), b.xyz(), c.xyz());
        float3 d = float3(a.w, b.w, c.w);
        return inverse(m) * d;
    };
    for (int i = 0; i < 8; i++ )
        outConsts.FrustumCorners[i] = float4(getCorner(i), 0);
}

void LightSamplingCache::updateLocalJitter()
{
    m_prevLocalJitter = m_localJitter;
    if (!m_dbgDebugDisableJitter)
    {
        // Advance R2 jitter sequence
        // http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/

        if ( (m_updateCounter % 1024) == 0 )
            m_localJitterF = { 0, 0 }; // not sure how long can the sequence remain high quality, so perhaps best to reset after a period

        static const float g = 1.32471795724474602596f;
        static const float a1 = 1.0f / g;
        static const float a2 = 1.0f / (g * g);
        m_localJitterF[0] = fmodf(m_localJitterF[0] + a1, 1.0f);
        m_localJitterF[1] = fmodf(m_localJitterF[1] + a2, 1.0f);

        m_localJitter = dm::clamp(uint2(m_localJitterF * (float)CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE), uint2(0, 0), uint2(CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE - 1, CAUSTICA_LIGHTING_SAMPLING_BUFFER_TILE_SIZE - 1));
    }
}

void LightSamplingCache::updateBegin(nvrhi::ICommandList* commandList, caustica::BindingCache & bindingCache, const UpdateSettings& _settings, double sceneTime, const std::shared_ptr<caustica::Scene>& scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, 
    std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer, std::vector<SubInstanceData>& subInstanceData, nvrhi::TextureHandle envMapProcessed)
{
    RAII_SCOPE( commandList->beginMarker("LightingUpdateBegin");, commandList->endMarker(); );
    // RAII_SCOPE( commandList->setEnableAutomaticBarriers(false);, commandList->setEnableAutomaticBarriers(true); );

    assert( _settings.FrameIndex != -1 && "forgot to set?" );

    assert(m_verifyBeginHasMatchingEnd == 0); m_verifyBeginHasMatchingEnd++; 

    m_ping = !m_ping;

    m_lastFrameIndex = m_currentSettings.FrameIndex;
    m_currentSettings = _settings;

    if (m_currentSettings.FrameIndex-1 != m_lastFrameIndex) // not sure if we want this
        m_currentSettings.ResetFeedback = true;

    if (m_currentSettings.ResetFeedback)
    {
        m_updateCounter = 0;
        m_localJitterF = { 0,0 };
        m_localJitter = m_prevLocalJitter = { 0,0 };
        m_NEE_AT_FeedbackBufferFilled = false;
    }

    updateLocalJitter();

    m_updateCounter++;

    bool lastFrameLocalSamplesAvailable = m_currentCtrlBuff.LastFrameTemporalFeedbackAvailable; // if last frame had temporal feedback, it will have had built local (tile) sampling

    m_currentSettings.GlobalTemporalFeedbackWeight  = dm::clamp(m_currentSettings.GlobalTemporalFeedbackWeight, 0.0f, 0.95f);
    m_currentSettings.LocalToGlobalSampleRatio      = dm::clamp(m_currentSettings.LocalToGlobalSampleRatio, 0.0f, 1.0f);
    if (m_currentSettings.ImportanceSamplingType != 2)  // no feedback needed if not using NEE_AT
    {
        m_currentSettings.GlobalTemporalFeedbackWeight = 0.0f;
        m_currentSettings.LocalToGlobalSampleRatio = 0.0f;
        lastFrameLocalSamplesAvailable = false;
        m_NEE_AT_FeedbackBufferFilled = false;
    }

    // Constants
    LightingControlData ctrlBuff; memset(&ctrlBuff, 0, sizeof(ctrlBuff)); 
    LightSamplingCacheConstants & cacheConsts = ctrlBuff.CacheConstants;

    updateFrustumConsts(cacheConsts, m_currentSettings);

    cacheConsts.UpdateCounter = m_updateCounter;
    cacheConsts.EnableMotionReprojection      = true;
    cacheConsts.DepthDisocclusionThreshold   = m_depthDisocclusionThreshold;
    ctrlBuff.LocalSamplingTileJitter     = m_localJitter;
    ctrlBuff.LocalSamplingTileJitterPrev = m_prevLocalJitter;

    assert( _settings.ViewportSize.x > 0 && _settings.ViewportSize.y > 0 && _settings.PrevViewportSize.x > 0 && _settings.PrevViewportSize.y > 0 );
    cacheConsts.PrevOverCurrentViewportSize = m_currentSettings.PrevViewportSize / m_currentSettings.ViewportSize;

    bool lastFrameFeedbackAvailable = m_NEE_AT_FeedbackBufferFilled && !m_dbgDebugDisableLastFrameFeedback;
    const bool temporalFeedbackRequired = m_currentSettings.ImportanceSamplingType == 2;

    {
        if( m_currentSettings.EnvMapParams.Enabled )
        {
            assert( envMapProcessed != nullptr );
            cacheConsts.EnvMapParams = m_currentSettings.EnvMapParams;
            const float baseScale = 0.0002f;
            cacheConsts.DistantVsLocalRelativeImportance = m_currentSettings.DistantVsLocalImportanceScale * baseScale;

            cacheConsts.EnvMapImportanceMapMIPCount = envMapProcessed->getDesc().mipLevels;
            cacheConsts.EnvMapImportanceMapResolution = envMapProcessed->getDesc().width; assert( envMapProcessed->getDesc().height == cacheConsts.EnvMapImportanceMapResolution );
            assert(m_envLightLookupMap != nullptr && m_envLightLookupMap->getDesc().width == cacheConsts.EnvMapImportanceMapResolution);
        }
        else
        {
            cacheConsts.DistantVsLocalRelativeImportance = 0.0f;
            cacheConsts.EnvMapParams = LightSamplingCacheEnvMapParams{ .Transform = float3x4::identity(), .InvTransform = float3x4::identity(), .ColorMultiplier = float3(1,1,1), .Enabled = 0.0f };
            cacheConsts.EnvMapImportanceMapMIPCount = 0;
            cacheConsts.EnvMapImportanceMapResolution = 0;
        }
    }

    cacheConsts.FeedbackResolution          = uint2(m_NEE_AT_FeedbackCandidates->getDesc().width, m_NEE_AT_FeedbackCandidates->getDesc().height);
    cacheConsts.BlendedFeedbackResolution   = uint2(m_NEE_AT_FeedbackCandidatesBlended->getDesc().width, m_NEE_AT_FeedbackCandidatesBlended->getDesc().height);
    uint numTotalP0ThreadCount              = div_ceil(cacheConsts.FeedbackResolution.x, LLB_NUM_COMPUTE_THREADS_2D) * div_ceil(cacheConsts.FeedbackResolution.y, LLB_NUM_COMPUTE_THREADS_2D) * LLB_NUM_COMPUTE_THREADS_2D * LLB_NUM_COMPUTE_THREADS_2D;
    ctrlBuff.TotalMaxFeedbackCount          = (lastFrameFeedbackAvailable)?(numTotalP0ThreadCount):(0);
    ctrlBuff.LocalSamplingResolution        = uint2(m_localSamplingBufferWidth, m_localSamplingBufferHeight);
    ctrlBuff.GlobalFeedbackUseWeight        = (lastFrameFeedbackAvailable) ? (m_currentSettings.GlobalTemporalFeedbackWeight): (0.0f);
    ctrlBuff.LocalToGlobalSampleRatio       = (lastFrameFeedbackAvailable) ? (m_currentSettings.LocalToGlobalSampleRatio) : (0.0f);
    cacheConsts.ReservoirHistoryDropoff     = m_advSetting_reservoirHistoryDropoff;
    ctrlBuff.ScreenSpaceVsWorldSpaceThreshold = m_advSetting_ScreenSpaceVsWorldSpaceThreshold;

    ctrlBuff.ImportanceSamplingType = m_currentSettings.ImportanceSamplingType;

    ctrlBuff.TileBufferHeight = ctrlBuff.LocalSamplingResolution.y;

    cacheConsts.DebugDrawType = (int)m_dbgDebugDrawType;
    cacheConsts.DebugDrawTileLights = m_dbgDebugDrawTileLightConnections;
    cacheConsts.MouseCursorPos = m_currentSettings.MouseCursorPos;
    cacheConsts.ImportanceBoostIntensityDelta = m_importanceBoost_IntensityDelta?m_importanceBoost_IntensityDeltaMul:0.0f;
    cacheConsts.ImportanceBoostFrustumMul = m_importanceBoost_Frustum?m_importanceBoost_FrustumMul:0.0f;
    cacheConsts.ImportanceBoostFrustumFadeDistance = m_importanceBoost_FrustumFadeDistance;
    ctrlBuff.LastFrameTemporalFeedbackAvailable = lastFrameFeedbackAvailable;
    cacheConsts.SceneCameraPos = m_currentSettings.CameraPosition;
    cacheConsts.SceneAverageContentsDistance = m_currentSettings.AverageContentsDistance;
    ctrlBuff.LastFrameLocalSamplesAvailable = lastFrameLocalSamplesAvailable && lastFrameFeedbackAvailable;
    ctrlBuff.LastFrameTemporalFeedbackAvailable = lastFrameFeedbackAvailable;
    ctrlBuff.TemporalFeedbackRequired = temporalFeedbackRequired && !m_dbgFreezeUpdates;

    // clear buffers
    m_scratchLightBuffer.clear(); m_scratchLightExBuffer.clear();
    m_scratchLightHistoryRemapCurrentToPastBuffer.clear();
    m_scratchLightHistoryRemapPastToCurrentBuffer.clear();
    // collect all environment lights (create placeholders to be filled on the GPU later)
    m_noOverflow = true;
    m_noOverflow &= collectEnvmapLightPlaceholders( m_currentSettings, ctrlBuff, m_scratchLightBuffer, m_scratchLightExBuffer, m_scratchLightHistoryRemapCurrentToPastBuffer, m_scratchLightHistoryRemapPastToCurrentBuffer );
    // collect all analytic lights
    m_noOverflow &= collectAnalyticLightsCPU( m_currentSettings, scene, ctrlBuff, m_scratchLightBuffer, m_scratchLightExBuffer, m_scratchLightHistoryRemapCurrentToPastBuffer, m_scratchLightHistoryRemapPastToCurrentBuffer );
    // inject 3DGS SH0/DC emission proxies as analytic sphere lights
    m_noOverflow &= collectGaussianSplatEmissionProxies( m_currentSettings, ctrlBuff, m_scratchLightBuffer, m_scratchLightExBuffer, m_scratchLightHistoryRemapCurrentToPastBuffer, m_scratchLightHistoryRemapPastToCurrentBuffer );
    // collect all emissive triangles and other geometry specific work - this builds batch jobs on the CPU that are executed on the GPU later, but at the end of this step we know the exact number of added emissive triangles (even though some might be black)
    m_noOverflow &= processEmissiveGeometry(m_currentSettings, scene, subInstanceData, ctrlBuff, *m_scratchTaskBuffer);
    cacheConsts.TriangleLightTaskCount = (int)(*m_scratchTaskBuffer).size();
    assert( ctrlBuff.EnvmapQuadNodeCount == CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT );
    assert( ctrlBuff.TotalLightCount == ctrlBuff.EnvmapQuadNodeCount + ctrlBuff.AnalyticLightCount + ctrlBuff.TriangleLightCount ); assert(ctrlBuff.TotalLightCount <= CAUSTICA_LIGHTING_MAX_LIGHTS);
    ctrlBuff.HistoricTotalLightCount = m_historicTotalLightCount;
    m_historicTotalLightCount = ctrlBuff.TotalLightCount;

    cacheConsts.CurrentWeightsBufferOffset  = (m_ping) ? 0 : CAUSTICA_LIGHTING_WEIGHTS_COUNT_HALF;
    cacheConsts.HistoricWeightsBufferOffset = (m_ping) ? CAUSTICA_LIGHTING_WEIGHTS_COUNT_HALF : 0;

    // Constant & control buffers must go first
    {
        RAII_SCOPE(commandList->beginMarker("ControlDataSetup");, commandList->endMarker(); );

        // upload control data
        // control buffer (used for build but also later for sampling)
        commandList->writeBuffer(m_controlBuffer, &ctrlBuff, sizeof(ctrlBuff));
        m_currentCtrlBuff = ctrlBuff;
        commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    // Bindings
    nvrhi::BindingSetDesc bindingSetDesc;
    fillBindings(bindingSetDesc, scene, materialGpuCache, opacityMicromapBuilder, subInstanceDataBuffer, nullptr, nullptr, envMapProcessed);
    nvrhi::BindingSetHandle bindingSet = bindingCache.getOrCreateBindingSet(bindingSetDesc, m_commonBindingLayout);

    nvrhi::BindingSetVector bindings = { bindingSet };
    nvrhi::BindingSetVector bindingsEx = { bindingSet, scene->getDescriptorTable() };

    {
        // we can do this early although we might have to move it to a later location if doing multiple global updates per frame (unlikely?)
        RAII_SCOPE(commandList->beginMarker("ResetLightProxyCounters"); , commandList->endMarker(); );

        const dm::uint  items = ctrlBuff.TotalLightCount;
        const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS;
        m_resetLightProxyCounters.execute(commandList, div_ceil(items, itemsPerGroup), 1, 1, bindingSet);
    }

    {
        // this is mostly for correctness/determinism - it will clean everything so any gaps in mapping to previous frame don't result in incorrect mapping
        RAII_SCOPE(commandList->beginMarker("ResetPastToCurrentHistory"); , commandList->endMarker(); );
        commandList->setBufferState(m_historyRemapPastToCurrentBuffer, nvrhi::ResourceStates::UnorderedAccess);
        m_resetPastToCurrentHistory.execute(commandList, div_ceil(std::max(ctrlBuff.HistoricTotalLightCount, ctrlBuff.TotalLightCount), LLB_NUM_COMPUTE_THREADS), 1, 1, bindingSet);
    }

    {
        RAII_SCOPE(commandList->beginMarker("EnvLightsBackupPast"); , commandList->endMarker(); );

        commandList->setBufferState(m_lightsBuffer, nvrhi::ResourceStates::UnorderedAccess); // very likely unnecessary in practice, but the old lightsBuffer is read in this pass
        m_envLightsBackupPast.execute(commandList, div_ceil(CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, LLB_NUM_COMPUTE_THREADS), 1, 1, bindingSet);
    }

    // empty emissive and analytic lights get copied over first - they've been fully processed on the CPU
    {
        RAII_SCOPE(commandList->beginMarker("EnvmapAndAnalyticLightBuffers");, commandList->endMarker(); );

        commandList->commitBarriers();
        assert( (int)m_scratchLightBuffer.size() == (ctrlBuff.EnvmapQuadNodeCount+ctrlBuff.AnalyticLightCount) );
        assert( (int)m_scratchLightExBuffer.size() == (ctrlBuff.EnvmapQuadNodeCount+ctrlBuff.AnalyticLightCount) );
        // TODO: setting all barriers before to copy_dest will potentially reduce gaps between copies
        commandList->writeBuffer(m_lightsBuffer, m_scratchLightBuffer.data(), sizeof(PolymorphicLightInfo) * m_scratchLightBuffer.size());
        commandList->writeBuffer(m_lightsExBuffer, m_scratchLightExBuffer.data(), sizeof(PolymorphicLightInfoEx)* m_scratchLightExBuffer.size());
        commandList->writeBuffer(m_historyRemapCurrentToPastBuffer, m_scratchLightHistoryRemapCurrentToPastBuffer.data(), sizeof(uint) * m_scratchLightHistoryRemapCurrentToPastBuffer.size());
        commandList->writeBuffer(m_historyRemapPastToCurrentBuffer, m_scratchLightHistoryRemapPastToCurrentBuffer.data(), sizeof(uint) * m_scratchLightHistoryRemapPastToCurrentBuffer.size());
        commandList->writeBuffer(m_scratchBuffer, m_scratchTaskBuffer->data(), sizeof(EmissiveTrianglesProcTask)* cacheConsts.TriangleLightTaskCount);
    }

    // todo: make sure only those needed are set
    commandList->setBufferState(m_perLightProxyCounters, nvrhi::ResourceStates::UnorderedAccess); // we've written into proxy counters - barrier needs to be added to the queue 
    commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    //commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    //commandList->setTextureState(m_NEE_AT_FeedbackCandidatesScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_lightsBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_lightsExBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_historyRemapCurrentToPastBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_historyRemapPastToCurrentBuffer, nvrhi::ResourceStates::UnorderedAccess);

    {
        RAII_SCOPE(commandList->beginMarker("EnvLightsSubdivideBase");, commandList->endMarker(); );
        m_envLightsSubdivideBase.execute(commandList, 1, 1, 1, bindingSet); //the main output goes to scratchBuffer, with CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT offset and is consumed by EnvLightsBake
    }
    
    {
        RAII_SCOPE(commandList->beginMarker("EnvLightsSubdivideBoost"); , commandList->endMarker(); );
        commandList->setBufferState(m_scratchList, nvrhi::ResourceStates::UnorderedAccess);
        m_envLightsSubdivideBoost.execute(commandList, CAUSTICA_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT, 1, 1, bindingSet); //the main output goes to scratchBuffer, with CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT offset and is consumed by EnvLightsBake
    }

    // We can probably overlap this with EnvLightsSubdivide but I measure no perf benefit
    {
        RAII_SCOPE(commandList->beginMarker("BakeEmissiveTriangles"); , commandList->endMarker(); );
        
        if (cacheConsts.TriangleLightTaskCount > 0)
            m_bakeEmissiveTriangles.execute(commandList, div_ceil(cacheConsts.TriangleLightTaskCount, 8), 1, 1, bindingsEx);

        commandList->setBufferState(m_lightsBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_historyRemapCurrentToPastBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_historyRemapPastToCurrentBuffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        RAII_SCOPE(commandList->beginMarker("EnvLightFillLookupMap"); , commandList->endMarker(); );
        
        commandList->setBufferState(m_lightsBuffer, nvrhi::ResourceStates::UnorderedAccess);

        m_envLightsFillLookupMap.execute(commandList, CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, 1, 1, bindings );
        
        commandList->setTextureState(m_envLightLookupMap, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        RAII_SCOPE(commandList->beginMarker("EnvLightsMapPastToCurrent"); , commandList->endMarker(); );

        commandList->setBufferState(m_scratchList, nvrhi::ResourceStates::UnorderedAccess);

        m_envLightsMapPastToCurrent.execute(commandList, div_ceil(CAUSTICA_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, LLB_NUM_COMPUTE_THREADS), 1, 1, bindings );

        commandList->setBufferState(m_scratchList, nvrhi::ResourceStates::UnorderedAccess);
    }

    // note: this has to come after all lights have been baked and remap current to past & past to current buffers are valid
    if (lastFrameFeedbackAvailable)
    {
        if( m_importanceBoost_PreFilter )
        {
            RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryPreFilter");, commandList->endMarker(); );

            m_processFeedbackHistoryPreFilter.execute(commandList, div_ceil(cacheConsts.FeedbackResolution.x, LLB_PREPROCESS_BLOCK_SIZE_INNER), div_ceil(cacheConsts.FeedbackResolution.y, LLB_PREPROCESS_BLOCK_SIZE_INNER), 1, bindings);
            commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        {
            RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryP0"); , commandList->endMarker(); );

            m_processFeedbackHistoryP0.execute(commandList, div_ceil(cacheConsts.FeedbackResolution.x, LLB_NUM_COMPUTE_THREADS_2D), div_ceil(cacheConsts.FeedbackResolution.y, LLB_NUM_COMPUTE_THREADS_2D), 1, bindings );

            commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);           // we've InterlockedAdd into u_controlBuffer (actually, we haven't, except in the validation verison, but leaving in for when enabling validation)
            commandList->setBufferState(m_perLightProxyCounters, nvrhi::ResourceStates::UnorderedAccess);   // we've InterlockedAdd into m_perLightProxyCounters
        }
    }

    {
        RAII_SCOPE(commandList->beginMarker("ComputeWeights"); , commandList->endMarker(); );

        const dm::uint  items = ctrlBuff.TotalLightCount;
        const dm::uint  itemsPerGroup = LLB_LOCAL_BLOCK_SIZE * LLB_NUM_COMPUTE_THREADS;

        commandList->setBufferState(m_historyRemapCurrentToPastBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_historyRemapPastToCurrentBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_lightWeights, nvrhi::ResourceStates::UnorderedAccess);

        m_computeWeights.execute(commandList, div_ceil(items, itemsPerGroup), 1, 1, bindingSet);

        commandList->setBufferState(m_lightWeights, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        RAII_SCOPE(commandList->beginMarker("ComputeProxyCounts"); , commandList->endMarker(); );

        const dm::uint  items = ctrlBuff.TotalLightCount;
        const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS;
        m_computeProxyCounts.execute(commandList, div_ceil(items, itemsPerGroup), 1, 1, bindingSet);

        commandList->setBufferState(m_perLightProxyCounters, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_scratchList, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        nvrhi::ComputeState state;
        state.bindings = { bindingSet };

        {
            RAII_SCOPE(commandList->beginMarker("ComputeProxyBaselineOffsets");, commandList->endMarker(); );

            m_computeProxyBaselineOffsets.execute(commandList, 1, 1, 1, bindingSet);

            commandList->setBufferState(m_lightSamplingProxies, nvrhi::ResourceStates::UnorderedAccess);
        }

        {
            RAII_SCOPE(commandList->beginMarker("CreateProxyJobs"); , commandList->endMarker(); );

            const dm::uint  items = ctrlBuff.TotalLightCount;
            const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS;
            m_createProxyJobs.execute(commandList, div_ceil(items, itemsPerGroup), 1, 1, bindingSet);

            commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);   // because we've written into u_controlBuffer[0].ProxyBuildTaskCount
            commandList->setBufferState(m_scratchBuffer, nvrhi::ResourceStates::UnorderedAccess);   // because this is where jobs are stored
        }
    }
    
    {
        RAII_SCOPE(commandList->beginMarker("ExecuteProxyJobs"); , commandList->endMarker(); );

        const dm::uint  items = LLB_MAX_PROXY_PROC_TASKS; // this one is updated on GPU so it's not correct here so let's just brute force to max, compute shader will skip...
        const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS;
        const dm::uint  dispatchCountX = div_ceil(items, itemsPerGroup); assert(dispatchCountX<=65535); // more than this triggers EXECUTION WARNING #1296: OVERSIZED_DISPATCH
        m_executeProxyJobs.execute(commandList, dispatchCountX, 1, 1, bindingSet);

        commandList->setBufferState(m_lightSamplingProxies, nvrhi::ResourceStates::UnorderedAccess);    // because we've filled it up
    }

    if( m_dbgDebugDrawLights )
    {
        RAII_SCOPE(commandList->beginMarker("DebugDrawLights"); , commandList->endMarker(); );

        commandList->setBufferState(m_controlBuffer, nvrhi::ResourceStates::UnorderedAccess);

        const dm::uint  items = CAUSTICA_LIGHTING_MAX_SAMPLING_PROXIES; // this one is updated on GPU so it's not correct here so let's just brute force to max, compute shader will skip...
        const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS;
        m_debugDrawLights.execute(commandList, div_ceil(items, itemsPerGroup), 1, 1, bindingSet);
    }

    // for debugging only
    if (m_framesFromLastReadbackCopy == -1)
        commandList->copyBuffer(m_controlBufferReadback, 0, m_controlBuffer, 0, sizeof(LightingControlData) * 1); // first time copy, do nothing else
    else
    {
#if LLB_ENABLE_VALIDATION   // instant feedback but significant perf hit
        m_device->waitForIdle(); 
#else
        if (m_framesFromLastReadbackCopy > 5) // 5 is always safe, we won't have that many frames overlapping
#endif
        {
            // Copy from readback buffer to struct that's displayed in UI
            void* pData = m_device->mapBuffer(m_controlBufferReadback, nvrhi::CpuAccessMode::Read);
            assert(pData);
            memcpy(&m_lastReadback, pData, sizeof(LightingControlData) * 1);
            m_device->unmapBuffer(m_controlBufferReadback);

            // Copy from GPU buffer to CPU readback buffer
            commandList->copyBuffer(m_controlBufferReadback, 0, m_controlBuffer, 0, sizeof(LightingControlData) * 1);

            // reset counter
            m_framesFromLastReadbackCopy = 0;
        }
    }
    commandList->commitBarriers();  // committing now avoids "D3D12 ERROR: ID3D12CommandList::ResourceBarrier: D3D12_RESOURCE_STATES has an invalid combination of state bits." later in DispatchRays; this needs to be debugged
    m_framesFromLastReadbackCopy++;
}

#define UAV_BARRIER_m_NEE_AT_LocalSamplingBuffer() { commandList->setBufferState(m_NEE_AT_LocalSamplingBuffer, nvrhi::ResourceStates::UnorderedAccess); }

void LightSamplingCache::updateEnd(nvrhi::ICommandList * commandList, caustica::BindingCache & bindingCache, const std::shared_ptr<caustica::Scene> & scene, std::shared_ptr<class MaterialGpuCache> materialGpuCache, std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder, nvrhi::BufferHandle subInstanceDataBuffer, nvrhi::TextureHandle depthBuffer, nvrhi::TextureHandle motionVectors)
{
    RAII_SCOPE(commandList->beginMarker("LightingUpdateEnd");, commandList->endMarker(); );

    if (m_currentSettings.ImportanceSamplingType != 2)  // no feedback or local sampling code needed if not using NEE_AT - just make sure you're not actually using local sampling!
        return;

    m_verifyBeginHasMatchingEnd--; assert( m_verifyBeginHasMatchingEnd == 0 );
    bool lastFrameFeedbackAvailable = m_NEE_AT_FeedbackBufferFilled;

    nvrhi::BindingSetDesc bindingSetDesc;
    fillBindings(bindingSetDesc, scene, materialGpuCache, opacityMicromapBuilder, subInstanceDataBuffer, depthBuffer, motionVectors, nullptr);
    nvrhi::BindingSetHandle bindingSet = bindingCache.getOrCreateBindingSet(bindingSetDesc, m_commonBindingLayout);
    nvrhi::BindingSetVector bindings = { bindingSet };

    const dm::uint  itemsPerGroup = LLB_NUM_COMPUTE_THREADS_2D;

    {
        RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryP1a"); , commandList->endMarker(); );

        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidatesScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_processFeedbackHistoryP1a.execute(commandList, div_ceil(m_currentCtrlBuff.CacheConstants.BlendedFeedbackResolution.x, itemsPerGroup), div_ceil(m_currentCtrlBuff.CacheConstants.BlendedFeedbackResolution.y, itemsPerGroup), 1, bindings);

        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidatesScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightBlended, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidatesBlended, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryP1b");, commandList->endMarker(); );

        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_processFeedbackHistoryP1b.execute(commandList, div_ceil(m_currentCtrlBuff.CacheConstants.FeedbackResolution.x, itemsPerGroup), div_ceil(m_currentCtrlBuff.CacheConstants.FeedbackResolution.y, itemsPerGroup), 1, bindings);

        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidatesScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }

    {
        RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryP2"); , commandList->endMarker(); );
        m_processFeedbackHistoryP2.execute(commandList, div_ceil(m_currentCtrlBuff.LocalSamplingResolution.x, itemsPerGroup), div_ceil(m_currentCtrlBuff.LocalSamplingResolution.y, itemsPerGroup), 1, bindings);
        UAV_BARRIER_m_NEE_AT_LocalSamplingBuffer();
    }

    {
        RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryP3"); , commandList->endMarker(); );
        m_processFeedbackHistoryP3.execute(commandList, m_currentCtrlBuff.LocalSamplingResolution.x, m_currentCtrlBuff.LocalSamplingResolution.y, 1, bindings);
        UAV_BARRIER_m_NEE_AT_LocalSamplingBuffer();
    }

    if (m_currentCtrlBuff.CacheConstants.DebugDrawTileLights || m_dbgDebugDrawType == LightingDebugViewType::TileHeatmap || m_dbgDebugDrawType == LightingDebugViewType::ValidateCorrectness || m_dbgFreezeFrustumUpdates)
    {
        UAV_BARRIER_m_NEE_AT_LocalSamplingBuffer();
        commandList->commitBarriers();

        RAII_SCOPE(commandList->beginMarker("ProcessFeedbackHistoryDebugViz"); , commandList->endMarker(); );
        m_processFeedbackHistoryDebugViz.execute(commandList, div_ceil(m_currentCtrlBuff.LocalSamplingResolution.x, itemsPerGroup), div_ceil(m_currentCtrlBuff.LocalSamplingResolution.y, itemsPerGroup), 1, bindings);

        UAV_BARRIER_m_NEE_AT_LocalSamplingBuffer();
        commandList->commitBarriers();
    }

    const bool temporalFeedbackRequired = m_currentSettings.ImportanceSamplingType == 2;
    if (m_currentCtrlBuff.TemporalFeedbackRequired)
    {
        RAII_SCOPE(commandList->beginMarker("ClearFeedbackHistory"); , commandList->endMarker(); );

        //commandList->setTextureState(m_NEE_AT_FeedbackTotalWeightScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        //commandList->setTextureState(m_NEE_AT_FeedbackCandidatesScratch, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_clearFeedbackHistory.execute( commandList, div_ceil(m_currentCtrlBuff.CacheConstants.FeedbackResolution.x, itemsPerGroup), div_ceil(m_currentCtrlBuff.CacheConstants.FeedbackResolution.y, itemsPerGroup), 1, bindings );

        commandList->setTextureState(m_NEE_AT_FeedbackTotalWeight, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_NEE_AT_FeedbackCandidates, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_NEE_AT_FeedbackBufferFilled = true;  // the assumption is that the path tracing happens after and actually fills the data; it's fine if it doesn't, the clear ^ resets it to empty anyway
    }

    // this is useful to avoid "leaking" any barrier issues to subsequent passes which makes it difficult to debug
    commandList->commitBarriers();
}

bool LightSamplingCache::infoGUI(float indent)
{
    RAII_SCOPE(ImGui::PushID("LightSamplingCacheInfoGUI");, ImGui::PopID(); );

    if (totalLightCountOverflow())
    {
        ImGui::TextColored({ 1,0.5f,0.5f,1 }, "!!WARNING - scene light count overflow!!");
        ImGui::TextColored({ 1,0.5f,0.5f,1 }, "increase CAUSTICA_LIGHTING_MAX_LIGHTS (%d)", CAUSTICA_LIGHTING_MAX_LIGHTS);
    }

    const char* modes[] = { "Uniform", "Power+", "NEE-AT" };
    ImGui::Text("Current mode:  %s", modes[dm::clamp(m_lastReadback.ImportanceSamplingType, 0u, 2u)]);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("As set in Path Tracer Next Event Estimation options\n(in the future, mode will be set here)");

    ImGui::Text("Scene lights by type: ");
    ImGui::Text("   envmap quads:  %d", m_lastReadback.EnvmapQuadNodeCount);
    ImGui::Text("   emissive tris: %d", m_lastReadback.TriangleLightCount);
    ImGui::Text("   analytic:      %d", m_lastReadback.AnalyticLightCount);
    ImGui::Text("   TOTAL:         %d", m_lastReadback.TotalLightCount);
    ImGui::Text("(used: %.2f%% of max %d)", (m_lastReadback.TotalLightCount / (float)CAUSTICA_LIGHTING_MAX_LIGHTS * 100.0f), CAUSTICA_LIGHTING_MAX_LIGHTS );
    ImGui::Text("(proxies: %d, weightsum: %.5f)", m_lastReadback.SamplingProxyCount, m_lastReadback.WeightsSum());
#if LLB_ENABLE_VALIDATION
    ImGui::Text("Validation:");
    float feedbackPerc = m_lastReadback.ValidFeedbackCount / float(m_currentCtrlBuff.CacheConstants.FeedbackResolution.x * m_currentCtrlBuff.CacheConstants.FeedbackResolution.y);
    ImGui::Text(" feedback num: %d (%.3f)", m_lastReadback.ValidFeedbackCount, feedbackPerc);
#endif
    float allocRam = (float)(double(m_allocatedVRAM)/1024.0/1024.0);
    ImGui::Text("Memory use: %.1f MiB", allocRam);

    return false;
}

bool LightSamplingCache::debugGUI(float indent)
{
    RAII_SCOPE(ImGui::PushID("LightSamplingCacheDebugGUI"); , ImGui::PopID(); );

    bool resetAccumulation = false;
    #define IMAGE_QUALITY_OPTION(code) do{if (code) resetAccumulation = true;} while(false)

    ImGui::Checkbox("Debug draw all lights", &m_dbgDebugDrawLights);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wireframe colour indicates type: red - environment map; green - emissive triangles; blue - analytic.");

    ImGui::Checkbox("Debug draw NEE-AT tile light connections", &m_dbgDebugDrawTileLightConnections);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows lights sampled by a specific tile local sampling pdf");

    ImGui::Checkbox("Freeze NEE-AT feedback updates", &m_dbgFreezeUpdates);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Feedback from the path tracer will remain frozen while this option is enabled.");

    const char* debugOptions = "Disabled\0Disocclusion\0NoHistoryFeedback\0MissingFeedbackScreenSpaceCoherent\0MissingFeedbackWorldSpaceCoherent\0FeedbackRawScreenSpaceCoherent\0FeedbackRawWorldSpaceCoherent\0LowResBlendedFeedback\0FeedbackAfterClear\0TileHeatmap\0ValidateCorrectness\0\0";
    ImGui::Combo("NEE-AT debug view", (int*)&m_dbgDebugDrawType, debugOptions);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show various NEE-AT buffers");

    ImGui::Checkbox("Debug disable local tile jitter", &m_dbgDebugDisableJitter);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mapping from pixels to tiles will be jittered to avoid denoising artifacts.\nIt also helps with spatial sharing.\nDisable for debugging.");

    ImGui::Checkbox("Debug disable last frame feedback", &m_dbgDebugDisableLastFrameFeedback);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Simply disables last frame's feedback for debugging/validation.\nQuality should revert to slightly worse than power based sampling.");
    
    ImGui::Checkbox("Debug freeze frustum updates", &m_dbgFreezeFrustumUpdates);

#if 1
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced settings", 0/*ImGuiTreeNodeFlags_DefaultOpen*/))
    {
        ImGui::SliderFloat("ScreenSpaceVsWorldSpaceThreshold", &m_advSetting_ScreenSpaceVsWorldSpaceThreshold, 0.02f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Used to determine, for each sampling location, whether it's more optimal to use screen tiles or world voxels for caching.");

        ImGui::SliderFloat("ReservoirHistoryDropoff", &m_advSetting_reservoirHistoryDropoff, 0.0f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The amount of history sharing from past and from neighbours. Some is useful, \ntoo much will add lag and allow strong lights to dwarf out others.");

        ImGui::SliderFloat("DepthDisocclusionThreshold", &m_depthDisocclusionThreshold, 0.999f, 20.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("During motion reprojection, drop samples if really far from target");

        ImGui::Checkbox("Sample environment proxy lights", &m_advSetting_SampleBakedEnvironment);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("If enabled, environment map texture will not be sampled directly by NEE\nbut will be baked into sampling proxies like emissive triangles.\nBiased, faster but more blurry shadows in some cases.");

        {
            ImGui::Text("Importance boosts:");
            RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent););
            ImGui::Checkbox("...by light intensity change", &m_importanceBoost_IntensityDelta);
            {
                RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent););
                RAII_SCOPE(ImGui::PushID("Delta");, ImGui::PopID(); );
                ImGui::InputFloat("multiplier", &m_importanceBoost_IntensityDeltaMul);
                m_importanceBoost_IntensityDeltaMul = dm::clamp(m_importanceBoost_IntensityDeltaMul, 0.0f, 1000.0f);
            }
            ImGui::Checkbox("...by light frustum proximity", &m_importanceBoost_Frustum);
            {
                RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
                RAII_SCOPE(ImGui::PushID("FrustProx"); , ImGui::PopID(); );
                ImGui::InputFloat("multiplier", &m_importanceBoost_FrustumMul);
                m_importanceBoost_FrustumMul = dm::clamp(m_importanceBoost_FrustumMul, 0.0f, 1000.0f);
                ImGui::InputFloat("fade distance", &m_importanceBoost_FrustumFadeDistance);
                m_importanceBoost_FrustumFadeDistance = dm::clamp(m_importanceBoost_FrustumFadeDistance, 0.0f, 1000.0f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("How fast the boost fades outside of the frustum\nThe bigger the value, the slower it fades");
                // ImGui::InputFloat("angle expand", &m_importanceBoost_FrustumAngleExpand);
                // m_importanceBoost_FrustumAngleExpand = dm::clamp(m_importanceBoost_FrustumAngleExpand, 0.0f, 1000.0f);
                // if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand frustum by specified number of degrees");
            }
            ImGui::Checkbox("...by pre-filter merge", &m_importanceBoost_PreFilter);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will allow stronger feedback in 3x3 kernel to 'overwhelm' neighbors\nEXPERIMENTAL - SUPER-SLOW");
        }
    }
#endif

    return resetAccumulation;
}

void LightSamplingCache::setGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros)
{
    macros.push_back({ "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", (m_advSetting_SampleBakedEnvironment) ? ("1") : ("0") });
}
