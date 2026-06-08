/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "caustica.h"

#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/View.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/json.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/float.h>
#include <donut/core/math/math.h>
#include <donut/shaders/light_cb.h>
#include <donut/shaders/view_cb.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "SampleCommon/PTPipelineBaker.h"
#include "SampleCommon/ComputePipelineBaker.h"

#include "SampleCommon/AccelerationStructureUtil.h"

#include "Lighting/Distant/EnvMapImportanceSamplingBaker.h"
#include "Materials/MaterialsBaker.h"

#include "OpacityMicroMap/OmmBaker.h"

#include "SampleCommon/LocalConfig.h"
#include "SampleCommon/SampleBaseApp.h"
#include "SampleCommon/CaptureScriptManager.h"
#include "Misc/Korgi.h"

#include "GPUSort/GPUSort.h"

#include "Misc/ZoomTool.h"

#include "ProcessingPasses/DenoisingGuidesBaker.h"
#include "ProcessingPasses/OidnDenoiser.h"
#include "ProcessingPasses/GaussianSplatPass.h"

#include <donut/engine/GltfImporter.h>
#include <donut/engine/SceneGraph.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include "SampleGame/GameScene.h"

#if RTXPT_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

#include <fstream>
#include <iostream>

#include <thread>

namespace
{
    constexpr const char* c_InlineSceneSentinel = "__RTXPT_INLINE_SCENE_JSON__";

    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }
}

#ifdef _WIN32
// Use discrete GPU by default on laptops.
extern "C"
{
    // http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;

    // https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
    constexpr float kGaussianSplatKernelMinResponse = 0.0113f;

    uint32_t ResolveGaussianSplatShadowMode(const SampleUIData& ui)
    {
        if (!ui.GaussianSplatShadows && ui.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = ui.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : ui.GaussianSplatShadowsMode;
        return uint32_t(std::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT));
    }

    uint32_t ClampGaussianSplatSoftShadowSamples(int sampleCount)
    {
        return uint32_t(std::clamp(sampleCount, 1, 16));
    }

    uint32_t ClampGaussianSplatEmissionProxyCount(int proxyCount)
    {
        return uint32_t(std::clamp(proxyCount, 0, 262144));
    }

    std::string MakeUniqueChildNodeName(const SceneGraphNode& parent, const std::string& desiredName)
    {
        const std::string baseName = desiredName.empty() ? "GaussianSplat" : desiredName;

        std::unordered_set<std::string> existingNames;
        for (size_t childIndex = 0; childIndex < parent.GetNumChildren(); childIndex++)
            existingNames.insert(parent.GetChild(childIndex)->GetName());

        if (existingNames.find(baseName) == existingNames.end())
            return baseName;

        for (uint32_t suffix = 2; ; suffix++)
        {
            std::string candidate = baseName + " (" + std::to_string(suffix) + ")";
            if (existingNames.find(candidate) == existingNames.end())
                return candidate;
        }
    }

    bool NodeSubtreeContains(SceneGraphNode* root, SceneGraphNode* candidate)
    {
        if (root == nullptr || candidate == nullptr)
            return false;

        SceneGraphWalker walker(root);
        while (walker)
        {
            if (walker.Get() == candidate)
                return true;
            walker.Next(true);
        }

        return false;
    }

    bool IsGaussianSplatEmissionEnabled(const SampleUIData& ui)
    {
        return ui.EnableGaussianSplats
            && ui.GaussianSplatAsEmitter
            && ui.GaussianSplatEmissionIntensity > 0.0f
            && ui.GaussianSplatEmissionMaxProxyCount > 0;
    }

#if RTXPT_WITH_NATIVE_DLSS
    float GetNativeDLSSResolutionScale(SI::DLSSMode mode)
    {
        switch (mode)
        {
        case SI::DLSSMode::eUltraPerformance: return 1.0f / 3.0f;
        case SI::DLSSMode::eMaxPerformance:   return 0.5f;
        case SI::DLSSMode::eBalanced:         return 0.58f;
        case SI::DLSSMode::eMaxQuality:       return 2.0f / 3.0f;
        case SI::DLSSMode::eUltraQuality:     return 0.77f;
        case SI::DLSSMode::eDLAA:             return 1.0f;
        default:                              return 0.58f;
        }
    }

    uint2 GetNativeDLSSRenderSize(uint2 displaySize, SI::DLSSMode mode)
    {
        const float scale = GetNativeDLSSResolutionScale(mode);
        return uint2(
            std::max(1u, uint32_t(std::round(float(displaySize.x) * scale))),
            std::max(1u, uint32_t(std::round(float(displaySize.y) * scale))));
    }
#endif

    float4x4 MakePinholeIntrinsicsProjection(float fx, float fy, float cx, float cy, float width, float height, float zNear)
    {
        width = std::max(width, 1.0f);
        height = std::max(height, 1.0f);

        const float xScale = 2.0f * fx / width;
        const float yScale = 2.0f * fy / height;
        const float xOffset = 2.0f * cx / width - 1.0f;
        const float yOffset = 1.0f - 2.0f * cy / height;

        return float4x4(
            xScale, 0.0f, 0.0f, 0.0f,
            0.0f, yScale, 0.0f, 0.0f,
            xOffset, yOffset, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f);
    }
}

#if defined(RTXPT_D3D_AGILITY_SDK_VERSION)
// Required for Agility SDK on Windows 10. Setup 1.c. 2.a.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = RTXPT_D3D_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

const char* g_windowTitle = "caustica";

const float c_envMapRadianceScale = 1.0f / 4.0f; // used to make input 32bit float radiance fit into 16bit float range that baker supports; going lower than 1/4 causes issues with current BC6U compression algorithm when used

static FPSLimiter g_FPSLimiter;

Sample::Sample(donut::app::DeviceManager& deviceManager,
    const CommandLineOptions& cmdLine)
    : app::ApplicationBase(&deviceManager )
    , m_cmdLine(cmdLine)
    , m_ui(g_sampleUIData)
{
    m_progressLoading.Start("Initializing...");
    m_progressLoading.Set(50);

    m_camera.SetRotateSpeed(.003f);

#if DONUT_WITH_STREAMLINE
    if (!GetDeviceManager()->GetDeviceParams().headlessDevice)
    {
        m_ui.IsDLSSSuported = GetDeviceManager()->GetStreamline().IsDLSSAvailable();
        m_ui.IsDLSSFGSupported = GetDeviceManager()->GetStreamline().IsDLSSGAvailable();
        m_ui.IsReflexSupported = GetDeviceManager()->GetStreamline().IsReflexAvailable();
        m_ui.IsDLSSRRSupported = GetDeviceManager()->GetStreamline().IsDLSSRRAvailable();
    }
#endif

    RefreshEnvironmentMapMediaList();

    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_ui, m_cmdLine);

#if RTXPT_WITH_PYTHON
    // Embedded Python scripting host - we always create the wrapper but the
    // interpreter itself is initialized on demand the first time a script
    // gets queued.  This keeps cold-start overhead at zero when scripting is
    // unused even if the executable was built with RTXPT_WITH_PYTHON=ON.
    m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
}

Sample::~Sample()
{
#if RTXPT_WITH_PYTHON
    // Tear down the Python interpreter first so that any nb::class_<>-bound
    // C++ objects (materials, lights, ...) are released while their owning
    // C++ data is still alive.
    m_pythonScripting.reset();
#endif
}

void Sample::DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 )
{
    if( int(m_cpuSideDebugLines.size())+2 >= MAX_DEBUG_LINES )
        return;
    DebugLineStruct dls = { float4(start, 1), col1 }, dle = { float4(stop, 1), col2 };
    m_cpuSideDebugLines.push_back(dls);
    m_cpuSideDebugLines.push_back(dle);
}

void Sample::Init(const std::string& preferredScene,
    const std::shared_ptr<donut::engine::ShaderFactory>& shaderFactory)
{
    m_shaderFactory = shaderFactory;

    m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_shaderFactory);
    m_bindingCache = std::make_unique<engine::BindingCache>(GetDevice());

#if RTXPT_WITH_NATIVE_DLSS
    m_nativeDLSS = donut::render::DLSS::Create(GetDevice(), *m_shaderFactory, donut::app::GetDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_ui.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_ui.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
        log::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_ui.IsDLSSSuported ? "yes" : "no",
            m_ui.IsDLSSRRSupported ? "yes" : "no");
    }
    else
    {
        log::warning("Native NGX DLSS object was not created.");
    }
#endif

    m_ui.EnableGaussianSplats = true;
    m_ui.GaussianSplatDepthTest = m_cmdLine.GaussianSplatDepthTest;
    m_ui.GaussianSplatScale = m_cmdLine.GaussianSplatScale;
    m_ui.GaussianSplatAlphaScale = m_cmdLine.GaussianSplatAlphaScale;
    m_ui.GaussianSplatBrightness = m_cmdLine.GaussianSplatBrightness;
    m_ui.GaussianSplatAsEmitter = m_cmdLine.GaussianSplatAsEmitter;
    m_ui.GaussianSplatEmissionIntensity = m_cmdLine.GaussianSplatEmissionIntensity;
    m_ui.GaussianSplatEmissionMaxProxyCount = m_cmdLine.GaussianSplatEmissionMaxProxyCount;
    m_ui.GaussianSplatAlphaCullThreshold = m_cmdLine.GaussianSplatAlphaCullThreshold;
    
    m_sampleGame = std::make_unique<GameScene>(*this, m_cmdLine);
    m_progressLoading.Set(95);

    nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
    bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
    bindlessLayoutDesc.firstSlot = 0;
    bindlessLayoutDesc.maxCapacity = 1024;
    bindlessLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2)
    };
    auto device = GetDevice();
    m_bindlessLayout = device->createBindlessLayout(bindlessLayoutDesc);

    nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
    globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
    globalBindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::PushConstants(1, sizeof(SampleMiniConstants)),
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        nvrhi::BindingLayoutItem::Texture_SRV(6),               // t_LdrColorScratch
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(7),     // GaussianSplatBVH
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),      // t_GaussianShadowSplats
        nvrhi::BindingLayoutItem::Texture_SRV(10),              // t_EnvironmentMap
        nvrhi::BindingLayoutItem::Texture_SRV(11),              // t_EnvironmentMapImportanceMap        <- TODO: remove this, no longer used
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12),     // t_LightsCB
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13),     // t_Lights
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),     // t_LightsEx
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(15),          // t_LightProxyCounters
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(16),          // t_LightProxyIndices
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(17),          // t_LightLocalSamplingBuffer
        nvrhi::BindingLayoutItem::Texture_SRV(18),              // t_EnvLookupMap
        nvrhi::BindingLayoutItem::Texture_UAV(20),              // u_LightFeedbackTotalWeight
        nvrhi::BindingLayoutItem::Texture_UAV(21),              // u_LightFeedbackCandidates
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Sampler(1),
        nvrhi::BindingLayoutItem::Sampler(2),
        nvrhi::BindingLayoutItem::Texture_UAV(0),           // u_OutputColor
        nvrhi::BindingLayoutItem::Texture_UAV(1),           // u_ProcessedOutputColor
        nvrhi::BindingLayoutItem::Texture_UAV(2),           // u_PostTonemapOutputColor
        nvrhi::BindingLayoutItem::Texture_UAV(4),           // u_Throughput
        nvrhi::BindingLayoutItem::Texture_UAV(5),           // u_MotionVectors
        nvrhi::BindingLayoutItem::Texture_UAV(6),           // u_Depth
        nvrhi::BindingLayoutItem::Texture_UAV(7),           // u_SpecularHitT
        nvrhi::BindingLayoutItem::Texture_UAV(8),           // u_ScratchFloat1
        // denoising slots go from 30-39
        //nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30), // denoiser 'control buffer' (might be removed, might be reused)
        nvrhi::BindingLayoutItem::Texture_UAV(31),          // RWTexture2D<float>  u_DenoiserViewspaceZ
        nvrhi::BindingLayoutItem::Texture_UAV(32),          // RWTexture2D<float4> u_DenoiserMotionVectors
        nvrhi::BindingLayoutItem::Texture_UAV(33),          // RWTexture2D<float4> u_DenoiserNormalRoughness
        nvrhi::BindingLayoutItem::Texture_UAV(34),          // RWTexture2D<float4> u_DenoiserDiffRadianceHitDist
        nvrhi::BindingLayoutItem::Texture_UAV(35),          // RWTexture2D<float4> u_DenoiserSpecRadianceHitDist
        nvrhi::BindingLayoutItem::Texture_UAV(36),          // RWTexture2D<float4> u_DenoiserDisocclusionThresholdMix
        nvrhi::BindingLayoutItem::Texture_UAV(37),          // RWTexture2D<float4> u_CombinedHistoryClampRelax
        // debugging slots go from 50-59
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(51),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(52),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(53),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(54),
        // ReSTIR GI
        nvrhi::BindingLayoutItem::Texture_UAV(60),          // u_SecondarySurfacePositionNormal
        nvrhi::BindingLayoutItem::Texture_UAV(61),          // u_SecondarySurfaceRadiance

        nvrhi::BindingLayoutItem::Texture_UAV(70),          // u_RRDiffuseAlbedo
        nvrhi::BindingLayoutItem::Texture_UAV(71),          // u_RRSpecAlbedo
        nvrhi::BindingLayoutItem::Texture_UAV(72),          // u_RRNormalsAndRoughness
        nvrhi::BindingLayoutItem::Texture_UAV(73),          // u_RRSpecMotionVectors
        nvrhi::BindingLayoutItem::Texture_UAV(74),          // u_RRTransparencyLayer
        nvrhi::BindingLayoutItem::Texture_UAV(75),          // u_DenoisingAvgLayerRadiance

        nvrhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        nvrhi::BindingLayoutItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX)
    };

    // NV HLSL extensions - DX12 only - we should probably expose some form of GetNvapiIsInitialized instead
    if (device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV))
    {
        globalBindingLayoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM));
    }

    // stable planes buffers -- must be last because these items are appended to the BindingSetDesc after the main list
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(40));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(42));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(44));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(45));

    // GBuffer
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(100));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(101));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(102));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(103));
    
    // Local cubemap UAV (for RT pass)
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(10));   // u_LocalCubemap

    // Reflection system textures
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(80));   // t_LocalCubemapGGX
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(81));   // t_DiffuseIrradianceCube
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(82));   // t_SSRBlurChain
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(83));   // t_BRDFLUT
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(84));   // t_DepthHierarchy
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(10)); // ReflectionConstants
    // SSRConstants removed - SSR now uses push constants instead of constant buffer
    
    // SSR result UAV (depth hierarchy UAVs u80-84 are in a dedicated binding set)
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(85));   // u_SSRResult

    // Ambient occlusion output consumed by deferred lighting
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(86));   // t_GTAOOutput
    // Previous frame depth (for temporal reprojection / disocclusion detection)
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(87));   // t_PrevDepth

    m_bindingLayout = device->createBindingLayout(globalBindingLayoutDesc);

    m_DescriptorTable = std::make_shared<engine::DescriptorTableManager>(device, m_bindlessLayout);

    auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
    m_TextureCache = std::make_shared<engine::TextureCache>(device, nativeFS, m_DescriptorTable);

    memset( &m_feedbackData, 0, sizeof(DebugFeedbackStruct) * 1 );
    memset( &m_debugDeltaPathTree, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices );

    //Draw lines from the feedback buffer - this is old and should all be replaced by ShaderDebug
    {
        std::vector<ShaderMacro> drawLinesMacro = { ShaderMacro("DRAW_LINES_SHADERS_OLD", "1") };
        m_linesVertexShader = m_shaderFactory->CreateShader("app/Misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, nvrhi::ShaderType::Vertex);
        m_linesPixelShader = m_shaderFactory->CreateShader("app/Misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, nvrhi::ShaderType::Pixel);

        nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setOffset(0)
                .setElementStride(sizeof(DebugLineStruct)),
                nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setOffset(offsetof(DebugLineStruct, col))
                .setElementStride(sizeof(DebugLineStruct)),
        };
        m_linesInputLayout = device->createInputLayout(attributes, uint32_t(std::size(attributes)), m_linesVertexShader);

        nvrhi::BindingLayoutDesc linesBindingLayoutDesc;
        linesBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        linesBindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0)
        };

        m_linesBindingLayout = device->createBindingLayout(linesBindingLayoutDesc);

        // debug stuff!
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = sizeof(DebugFeedbackStruct) * 1;
            bufferDesc.isConstantBuffer = false;
            bufferDesc.isVolatile = false;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
            bufferDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
            bufferDesc.structStride = sizeof(DebugFeedbackStruct);
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::Common;
            bufferDesc.debugName = "Feedback_Buffer_Gpu";
            m_feedback_Buffer_Gpu = device->createBuffer(bufferDesc);

            bufferDesc.canHaveUAVs = false;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            bufferDesc.structStride = 0;
            bufferDesc.keepInitialState = false;
            bufferDesc.initialState = nvrhi::ResourceStates::Unknown;
            bufferDesc.debugName = "Feedback_Buffer_Cpu";
            m_feedback_Buffer_Cpu = device->createBuffer(bufferDesc);

            bufferDesc.byteSize = sizeof(DebugLineStruct) * MAX_DEBUG_LINES;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.isConstantBuffer = false;
            bufferDesc.isVolatile = false;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
            bufferDesc.structStride = sizeof(DebugLineStruct);
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::Common;
            bufferDesc.debugName = "DebugLinesCapture";
            m_debugLineBufferCapture    = device->createBuffer(bufferDesc);
            bufferDesc.debugName = "DebugLinesDisplay";
            m_debugLineBufferDisplay    = device->createBuffer(bufferDesc);

            bufferDesc.byteSize = sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices;
            bufferDesc.isConstantBuffer = false;
            bufferDesc.isVolatile = false;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
            bufferDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
            bufferDesc.structStride = sizeof(DeltaTreeVizPathVertex);
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::Common;
            bufferDesc.debugName = "Feedback_PathDecomp_Gpu";
            m_debugDeltaPathTree_Gpu = device->createBuffer(bufferDesc);

            bufferDesc.canHaveUAVs = false;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            bufferDesc.structStride = 0;
            bufferDesc.keepInitialState = false;
            bufferDesc.initialState = nvrhi::ResourceStates::Unknown;
            bufferDesc.debugName = "Feedback_PathDecomp_Cpu";
            m_debugDeltaPathTree_Cpu = device->createBuffer(bufferDesc);


            bufferDesc.byteSize = sizeof(PathPayload) * cDeltaTreeVizMaxStackSize;
            bufferDesc.isConstantBuffer = false;
            bufferDesc.isVolatile = false;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
            bufferDesc.maxVersions = engine::c_MaxRenderPassConstantBufferVersions;
            bufferDesc.structStride = sizeof(PathPayload);
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::Common;
            bufferDesc.debugName = "DebugDeltaPathTreeSearchStack";
            m_debugDeltaPathTreeSearchStack = device->createBuffer(bufferDesc);
        }
    }

    // Main constant buffer
    m_constantBuffer = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(SampleConstants), "SampleConstants", engine::c_MaxRenderPassConstantBufferVersions*2));	// *2 because in some cases we update twice per frame

    // Command list!
    m_commandList = device->createCommandList();

    if(device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        m_ommBaker = std::make_shared<OmmBaker>(device, m_DescriptorTable, m_TextureCache, m_shaderFactory);

    // Get all scenes in "assets" folder
    const std::string mediaExt = ".scene.json";
    const std::string jsonExt = ".json";
    const std::filesystem::path assetsPath = GetLocalPath(c_AssetsFolder);
    if (std::filesystem::exists(assetsPath) && std::filesystem::is_directory(assetsPath))
    {
        for (const auto& file : std::filesystem::directory_iterator(assetsPath))
        {
            if (!file.is_regular_file()) continue;
            std::string fileName = file.path().filename().string();
            std::string longExt = (fileName.size()<=mediaExt.length())?(""):(fileName.substr(fileName.length()-mediaExt.length()));
            std::string shortExt = (fileName.size() <= jsonExt.length()) ? ("") : (fileName.substr(fileName.length() - jsonExt.length()));
            if ( longExt == mediaExt || shortExt == jsonExt )
                m_sceneFilesAvailable.push_back( file.path().filename().string() );
        }
    }

    std::string scene;
    if (LooksLikeInlineSceneJson(preferredScene))
    {
        scene = preferredScene;
    }
    else
    {
        std::filesystem::path preferredScenePath(preferredScene);
        scene = (!preferredScene.empty() && (preferredScenePath.is_absolute() || std::filesystem::exists(preferredScenePath)))
            ? preferredScene
            : FindPreferredScene(m_sceneFilesAvailable, preferredScene);
    }

    // Select initial scene
    SetCurrentScene(scene);
}

void Sample::SetCurrentScene( const std::string & sceneName, bool forceReload )
{
    if( m_currentSceneName == sceneName && !forceReload )
        return;
    m_currentSceneName = sceneName;
    m_ui.ResetAccumulation = true;
    SetAsynchronousLoadingEnabled( false );

    const bool inlineScene = LooksLikeInlineSceneJson(sceneName);
    if (inlineScene)
        m_inlineSceneJson = sceneName;
    else
        m_inlineSceneJson.clear();

    std::filesystem::path scenePath = inlineScene
        ? std::filesystem::path(c_InlineSceneSentinel)
        : std::filesystem::path(sceneName);
    if (!inlineScene && !scenePath.is_absolute() && !std::filesystem::exists(scenePath))
        scenePath = GetLocalPath(c_AssetsFolder) / scenePath;
    m_currentScenePath = scenePath;
    m_progressLoading.Stop();
    m_progressLoading.Start("Loading scene...");
    BeginLoadingScene( std::make_shared<vfs::NativeFileSystem>(), scenePath );
    if( m_scene == nullptr )
    {
        log::error( "Unable to load scene '%s'", sceneName.c_str() );
        m_currentScenePath = std::filesystem::path();
        m_progressLoading.Stop();
        return;
    }
}

bool Sample::LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToDonut)
{
    return AttachGaussianSplatToScene(fileName, convertRdfToDonut);
}

std::filesystem::path Sample::ResolveGaussianSplatPath(const GaussianSplat& splat) const
{
    if (splat.path.empty())
        return {};

    std::filesystem::path splatPath = splat.path;
    if (splatPath.is_absolute())
        return splatPath;

    const std::filesystem::path sceneFolder = m_currentScenePath.parent_path();
    if (!sceneFolder.empty() && m_currentScenePath != std::filesystem::path(c_InlineSceneSentinel))
        return sceneFolder / splatPath;

    return std::filesystem::absolute(splatPath);
}

void Sample::PrepareGaussianSplatPass(GaussianSplatPass& pass)
{
    if (m_renderTargets == nullptr || m_shaderDebug == nullptr)
        return;

    if (m_gpuSort == nullptr)
        m_gpuSort = std::make_shared<GPUSort>(GetDevice(), m_shaderFactory);
    m_gpuSort->CreateRenderPasses(m_CommonPasses, m_shaderDebug);
    pass.SetGpuSort(m_gpuSort);
    pass.CreatePipeline(*m_renderTargets);
}

uint32_t Sample::GetTotalGaussianSplatCount() const
{
    uint64_t total = 0;
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.pass != nullptr)
            total += object.pass->GetSplatCount();
    }
    return uint32_t(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

void Sample::UpdateGaussianSplatUIState()
{
    m_ui.GaussianSplatObjectCount = uint32_t(m_gaussianSplatSceneObjects.size());
    m_ui.GaussianSplatCount = GetTotalGaussianSplatCount();

    m_gaussianSplatFileNameSummary.clear();
    if (m_gaussianSplatSceneObjects.size() == 1 && m_gaussianSplatSceneObjects.front().pass != nullptr)
    {
        m_gaussianSplatFileNameSummary = m_gaussianSplatSceneObjects.front().pass->GetSourceFileName();
    }
    else if (!m_gaussianSplatSceneObjects.empty())
    {
        m_gaussianSplatFileNameSummary = std::to_string(m_gaussianSplatSceneObjects.size()) + " scene Gaussian Splat objects";
    }
    m_ui.GaussianSplatFileName = m_gaussianSplatFileNameSummary;

    if (m_ui.GaussianSplatObjectCount == 0)
        m_ui.SelectedGaussianSplat = false;
}

void Sample::LoadGaussianSplatsFromScene()
{
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();

    if (!m_scene || !m_scene->GetSceneGraph() || !m_shaderFactory)
    {
        UpdateGaussianSplatUIState();
        return;
    }

    SceneGraphWalker walker(m_scene->GetSceneGraph()->GetRootNode().get());
    while (walker)
    {
        auto splat = std::dynamic_pointer_cast<GaussianSplat>(walker->GetLeaf());
        if (splat != nullptr)
        {
            splat->loadedSplatCount = 0;
            splat->resolvedPath.clear();

            const std::filesystem::path splatPath = ResolveGaussianSplatPath(*splat);
            if (splatPath.empty())
            {
                log::error("Gaussian Splat node '%s' has no path/file field.", walker->GetName().c_str());
            }
            else
            {
                auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
                if (pass->LoadFromFile(splatPath, splat->convertRdfToDonut))
                {
                    splat->resolvedPath = splatPath.string();
                    splat->loadedSplatCount = pass->GetSplatCount();
                    PrepareGaussianSplatPass(*pass);

                    GaussianSplatSceneObject object;
                    object.splat = splat;
                    object.node = walker->shared_from_this();
                    object.pass = std::move(pass);
                    m_gaussianSplatSceneObjects.push_back(std::move(object));
                }
                else
                {
                    log::error("Failed to load Gaussian Splat node '%s' from '%s'.",
                        walker->GetName().c_str(), splatPath.string().c_str());
                }
            }
        }

        walker.Next(true);
    }

    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
}

bool Sample::AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToDonut)
{
    if (!m_scene || !m_scene->GetSceneGraph() || !m_scene->GetSceneGraph()->GetRootNode())
    {
        log::error("Cannot load Gaussian splats before a scene is loaded.");
        return false;
    }
    if (!m_shaderFactory)
    {
        log::error("Cannot load Gaussian splats before the shader factory is initialized.");
        return false;
    }

    std::filesystem::path splatPath = fileName;
    if (!splatPath.is_absolute())
        splatPath = std::filesystem::absolute(splatPath);

    if (!std::filesystem::exists(splatPath))
    {
        log::error("Gaussian Splat file does not exist: '%s'", splatPath.string().c_str());
        return false;
    }

    auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
    if (!pass->LoadFromFile(splatPath, convertRdfToDonut))
    {
        log::error("Failed to load Gaussian Splat file '%s'.", splatPath.string().c_str());
        return false;
    }
    if (pass->GetSplatCount() == 0)
    {
        log::error("Gaussian Splat file '%s' contains no splats.", splatPath.string().c_str());
        return false;
    }

    auto splat = std::make_shared<GaussianSplat>();
    splat->path = splatPath.string();
    splat->resolvedPath = splatPath.string();
    splat->convertRdfToDonut = convertRdfToDonut;
    splat->enabled = true;
    splat->loadedSplatCount = pass->GetSplatCount();

    auto node = std::make_shared<SceneGraphNode>();
    auto sceneGraph = m_scene->GetSceneGraph();
    auto rootNode = sceneGraph->GetRootNode();
    node->SetName(MakeUniqueChildNodeName(*rootNode, splatPath.filename().string()));
    node->SetLeaf(splat);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    node->SetTranslation(dm::double3(
        double(m_ui.GaussianSplatTranslation.x),
        double(m_ui.GaussianSplatTranslation.y),
        double(m_ui.GaussianSplatTranslation.z)));
    node->SetRotation(dm::rotationQuat(dm::double3(
        double(m_ui.GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(m_ui.GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(m_ui.GaussianSplatRotationEulerDeg.z) * deg2rad)));
    node->SetScaling(dm::double3(
        double(m_ui.GaussianSplatObjectScale.x),
        double(m_ui.GaussianSplatObjectScale.y),
        double(m_ui.GaussianSplatObjectScale.z)));

    auto attachedNode = sceneGraph->Attach(rootNode, node);
    m_scene->RefreshSceneGraph(GetFrameIndex());

    PrepareGaussianSplatPass(*pass);

    GaussianSplatSceneObject object;
    object.splat = splat;
    object.node = attachedNode;
    object.pass = std::move(pass);
    m_gaussianSplatSceneObjects.push_back(std::move(object));

    m_ui.EnableGaussianSplats = true;
    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
    RequestFullRebuild();
    return true;
}

uint32_t Sample::GetGaussianSplatCount() const
{
    return GetTotalGaussianSplatCount();
}

uint32_t Sample::GetGaussianSplatObjectCount() const
{
    return uint32_t(m_gaussianSplatSceneObjects.size());
}

const std::string& Sample::GetGaussianSplatFileName() const
{
    return m_gaussianSplatFileNameSummary;
}

Sample::GaussianSplatSceneObject* Sample::GetPrimaryGaussianSplatObject()
{
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

const Sample::GaussianSplatSceneObject* Sample::GetPrimaryGaussianSplatObject() const
{
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

float4x4 Sample::GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const
{
    auto node = object.node.lock();
    if (node == nullptr)
        return float4x4::identity();

    return dm::affineToHomogeneous(node->GetLocalToWorldTransformFloat());
}

void Sample::BuildGaussianSplatEmissionProxyList()
{
    m_gaussianSplatEmissionProxies.clear();

    if (!IsGaussianSplatEmissionEnabled(m_ui))
        return;

    const uint32_t maxProxyCount = ClampGaussianSplatEmissionProxyCount(m_ui.GaussianSplatEmissionMaxProxyCount);
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > m_gaussianSplatEmissionProxies.size()
            ? maxProxyCount - uint32_t(m_gaussianSplatEmissionProxies.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        object.pass->BuildEmissionProxies(
            remainingProxyCount,
            m_ui.GaussianSplatScale,
            uint32_t(std::clamp(m_ui.GaussianSplatRtxKernelDegree, 0, 5)),
            m_ui.GaussianSplatRtxAdaptiveClamp,
            m_ui.GaussianSplatTintColor,
            m_ui.GaussianSplatAlphaCullThreshold);

        auto node = object.node.lock();
        const dm::affine3 objectToWorld = node != nullptr
            ? node->GetLocalToWorldTransformFloat()
            : dm::affine3::identity();

        const float radiusScale = std::max({
            length(objectToWorld.transformVector(float3(1.0f, 0.0f, 0.0f))),
            length(objectToWorld.transformVector(float3(0.0f, 1.0f, 0.0f))),
            length(objectToWorld.transformVector(float3(0.0f, 0.0f, 1.0f))) });

        const auto& proxies = object.pass->GetEmissionProxies();
        m_gaussianSplatEmissionProxies.reserve(m_gaussianSplatEmissionProxies.size() + proxies.size());
        for (const GaussianSplatEmissionProxy& proxy : proxies)
        {
            GaussianSplatEmissionProxy transformed = proxy;
            transformed.center = objectToWorld.transformPoint(proxy.center);
            transformed.radius = proxy.radius * radiusScale;
            m_gaussianSplatEmissionProxies.push_back(transformed);
        }
    }
}

void Sample::SceneUnloading( )
{
    m_ui.TogglableNodes = nullptr;
    ApplicationBase::SceneUnloading();
    m_bindingSet = nullptr;
    m_topLevelAS = nullptr;
    m_subInstanceBuffer = nullptr;
    m_bindingCache->Clear( );
    m_lights.clear();
    m_ui.SelectedMaterial = nullptr;
    m_ui.SelectedNode = nullptr;
    m_ui.SelectedGaussianSplat = false;
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();
    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
    m_ui.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapBaker = nullptr;
    m_lightsBaker = nullptr;
    m_materialsBaker = nullptr;
    m_gpuSort = nullptr;
    m_uncompressedTextures.clear();
    if (m_rtxdiPass != nullptr) 
	    m_rtxdiPass->Reset();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneUnloading();

    DestroyRTPipelines();
    m_ptPipelineBaker = nullptr;
    m_computePipelineBaker = nullptr;

    if (m_sampleGame!=nullptr) m_sampleGame->SceneUnloading();
}

bool Sample::LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName)
{
    m_scene = std::shared_ptr<ExtendedScene>( new ExtendedScene(GetDevice(), *m_shaderFactory, fs, m_TextureCache, m_DescriptorTable, std::make_shared<ExtendedSceneTypeFactory>() ) );
    m_progressLoading.Set(10);
    if (sceneFileName == std::filesystem::path(c_InlineSceneSentinel))
    {
        if (m_scene->LoadFromJsonString(m_inlineSceneJson))
            return true;
        m_scene = nullptr;
        return false;
    }
    if (m_scene->Load(sceneFileName))
        return true;
    m_scene = nullptr;
    return false;
}

void Sample::UpdateCameraFromScene( const std::shared_ptr<donut::engine::PerspectiveCamera> & sceneCamera )
{
    dm::affine3 viewToWorld = sceneCamera->GetViewToWorldMatrix();
    dm::float3 cameraPos = viewToWorld.m_translation;
    m_camera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    m_cameraVerticalFOV = sceneCamera->verticalFov;
    m_cameraZNear = sceneCamera->zNear;

    std::shared_ptr<PerspectiveCameraEx> sceneCameraEx = std::dynamic_pointer_cast<PerspectiveCameraEx>(sceneCamera);
    if( sceneCameraEx != nullptr )
    {
        ToneMappingParameters defaults;

        m_ui.ToneMappingParams.autoExposure = sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
        m_ui.ToneMappingParams.exposureCompensation = sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
        m_ui.ToneMappingParams.exposureValue = sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
        m_ui.ToneMappingParams.exposureValueMin = sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
        m_ui.ToneMappingParams.exposureValueMax = sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
    }
}

void Sample::UpdateViews( nvrhi::IFramebuffer* framebuffer )
{
    // we currently use TAA for jitter even when it's not used itself
    if (m_temporalAntiAliasingPass)
        m_temporalAntiAliasingPass->SetJitter(m_ui.TemporalAntiAliasingJitter);

    nvrhi::Viewport windowViewport(float(m_renderSize.x), float(m_renderSize.y));
    m_view->SetViewport(windowViewport);
    float outputAspectRatio = m_displayAspectRatio; //windowViewport.width() / windowViewport.height();    // render and display outputs might not match in case of lower DLSS/etc resolution rounding!
    const float4x4 projection = m_cameraUseCustomIntrinsics
        ? MakePinholeIntrinsicsProjection(
            m_cameraIntrinsics.x,
            m_cameraIntrinsics.y,
            m_cameraIntrinsics.z,
            m_cameraIntrinsics.w,
            m_cameraIntrinsicsViewport.x,
            m_cameraIntrinsicsViewport.y,
            m_cameraZNear)
        : perspProjD3DStyleReverse(m_cameraVerticalFOV, outputAspectRatio, m_cameraZNear);
    m_view->SetMatrices(m_camera.GetWorldToViewMatrix(), projection);
    m_view->SetPixelOffset(ComputeCameraJitter(m_sampleIndex));
    m_view->UpdateCache();
    if ((m_frameIndex & 0xFFFFFFFF) == 0)
    {
        m_viewPrevious->SetMatrices(m_view->GetViewMatrix(), m_view->GetProjectionMatrix());
        m_viewPrevious->SetPixelOffset(m_view->GetPixelOffset());
        m_viewPrevious->UpdateCache();
    }
}

void Sample::CollectUncompressedTextures()
{
    // Make a list of uncompressed textures
    m_uncompressedTextures.clear();
    auto listUncompressedTextureIfNeeded = [ & ](std::shared_ptr<LoadedTexture> texture, bool normalMap)//, TextureCompressionType compressionType)
    {
        if (texture == nullptr || texture->texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->texture->getDesc();
        if (nvrhi::getFormatInfo(desc.format).blockSize != 1) // it's compressed, everything is fine!
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (nvrhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

        auto it = m_uncompressedTextures.insert(std::make_pair(texture, compressionType));
        if (!it.second)
        {
            assert(it.first->second == compressionType); // not the same compression type? that's bad!
            return;
        }
    };
    for ( auto textureIT : m_materialsBaker->GetUsedTextures() )
        listUncompressedTextureIfNeeded(textureIT.second.Loaded, textureIT.second.NormalMap);
}

static bool IsEnvironmentMapMediaFile(const std::filesystem::path& path)
{
    if (!path.has_filename())
        return false;
    const std::string ext = path.extension().string();
    return ext == ".exr" || ext == ".hdr" || ext == ".dds";
}

static void AppendEnvironmentMapsFromFolder(
    const std::filesystem::path& folder,
    std::vector<std::filesystem::path>& outList)
{
    if (folder.empty() || !std::filesystem::exists(folder))
        return;

    for (const auto& file : std::filesystem::directory_iterator(folder))
    {
        if (!file.is_regular_file() || !IsEnvironmentMapMediaFile(file.path()))
            continue;

        const std::filesystem::path absolutePath = std::filesystem::absolute(file.path());
        if (std::find(outList.begin(), outList.end(), absolutePath) == outList.end())
            outList.push_back(absolutePath);
    }
}

void Sample::RefreshEnvironmentMapMediaList()
{
    m_envMapMediaList.clear();

    std::filesystem::path sceneDirectory;
    if (!m_currentScenePath.empty() && m_currentScenePath != std::filesystem::path(c_InlineSceneSentinel))
        sceneDirectory = m_currentScenePath.parent_path();

    const std::filesystem::path assetsRoot = GetLocalPath(c_AssetsFolder);
    const std::filesystem::path sceneEnvFolder = sceneDirectory / c_EnvMapSubFolder;
    const std::filesystem::path assetsEnvFolder = assetsRoot / c_EnvMapSubFolder;

    // Runtime Assets/ first, then scene JSON directory (same as texture / EnvironmentLight.path).
    AppendEnvironmentMapsFromFolder(assetsEnvFolder, m_envMapMediaList);
    AppendEnvironmentMapsFromFolder(sceneEnvFolder, m_envMapMediaList);

    if (std::filesystem::exists(assetsEnvFolder))
        m_envMapMediaFolder = assetsEnvFolder;
    else
        m_envMapMediaFolder = sceneEnvFolder;
}

void Sample::SceneLoaded( )
{
    m_frameIndex = 0;

    RefreshEnvironmentMapMediaList();

    m_progressLoading.Set(50);

    if (m_sampleGame != nullptr) m_sampleGame->SceneLoaded(m_scene, m_currentScenePath, GetLocalPath(c_AssetsFolder));

    m_progressLoading.Set(55);

    ApplicationBase::SceneLoaded( );

    m_progressLoading.Set(60);

    m_sceneTime = 0.f;
    m_scene->FinishedLoading( GetFrameIndex( ) );

    LoadGaussianSplatsFromScene();

    if (!m_initialGaussianSplatAttached && !m_cmdLine.GaussianSplatFileName.empty())
    {
        m_initialGaussianSplatAttached = true;
        (void)AttachGaussianSplatToScene(m_cmdLine.GaussianSplatFileName, m_cmdLine.GaussianSplatConvertRdfToDonut);
    }

    m_progressLoading.Set(65);

	// Find lights; do this before special cases to avoid duplicates
	for (auto light : m_scene->GetSceneGraph()->GetLights())
	{
		m_lights.push_back(light);
	}

    // seem like sensible defaults
    m_ui.ToneMappingParams.exposureCompensation = 2.0f;
    m_ui.ToneMappingParams.exposureValue = 0.0f;

    std::shared_ptr<EnvironmentLight> envLight = FindEnvironmentLight(m_lights);
    m_envMapLocalPath = (envLight==nullptr)?(""):(envLight->path);
    m_ui.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;

    if (m_ui.TogglableNodes == nullptr)
    {
        m_ui.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*m_ui.TogglableNodes, GetScene()->GetSceneGraph()->GetRootNode().get()); // UNSAFE - make sure not to keep m_ui.TogglableNodes longer than scenegraph!
    }

    // clean up invisible lights / markers because they slow things down
    for (int i = (int)m_lights.size() - 1; i >= 0; i--)
    {
        LightConstants lc;
        m_lights[i]->FillLightConstants(lc);
        if (length(lc.color * lc.intensity) <= 1e-7f)
            m_lights.erase( m_lights.begin() + i );
    }

    if( m_envMapLocalPath != "" )
    {
        // Make sure that there's an environment light object attached to the scene,
        // so that RTXDI will pick it up and sample.
        if (envLight == nullptr)
        {
            envLight = std::make_shared<EnvironmentLight>();
            m_scene->GetSceneGraph()->AttachLeafNode(m_scene->GetSceneGraph()->GetRootNode(), envLight);
            m_lights.push_back(envLight);
        }
    }

    // setup camera - just load the last from the scene if available
    auto cameras = m_scene->GetSceneGraph( )->GetCameras( );
    auto camScene = (cameras.empty( ))?(nullptr):(std::dynamic_pointer_cast<PerspectiveCamera>(cameras.back()));

    if( camScene == nullptr )
    {
        m_camera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.55f, 0.f), float3(0, 1, 0));
        m_cameraVerticalFOV = dm::radians(60.0f);
        m_cameraZNear = 0.001f;
    }
    else
    {
        UpdateCameraFromScene( camScene );
    }

    // mark raytracing acceleration structures as dirty
    m_ui.AccelerationStructRebuildRequested = true;
    m_subInstanceCount = 0;

    // if we don't re-set these, BLAS-es for animated stuff don't get updated
    for( const auto& anim : m_scene->GetSceneGraph( )->GetAnimations( ) )
        (void)anim->Apply( 0.0f );

    // PrintSceneGraph( m_scene->GetSceneGraph( )->GetRootNode( ) );

    m_ui.ShaderReloadRequested = true;  // we have to re-create shader hit table
    m_ui.EnableAnimations = false;
    m_ui.RealtimeMode = false;

    std::shared_ptr<SampleSettings> settings = m_scene->GetSampleSettingsNode();
    if (settings != nullptr)
    {
        m_ui.RealtimeMode = settings->realtimeMode.value_or(m_ui.RealtimeMode);
        m_ui.EnableAnimations = settings->enableAnimations.value_or(m_ui.EnableAnimations);
        if (settings->startingCamera.has_value())
            m_selectedCameraIndex = settings->startingCamera.value()+1; // slot 0 reserved for free flight camera
        if (settings->realtimeFireflyFilter.has_value())
        {
            m_ui.RealtimeFireflyFilterThreshold = settings->realtimeFireflyFilter.value();
            m_ui.RealtimeFireflyFilterEnabled = true;
        }
        m_ui.BounceCount = settings->maxBounces.value_or(m_ui.BounceCount);
        m_ui.DiffuseBounceCount = settings->maxDiffuseBounces.value_or(m_ui.DiffuseBounceCount);
        m_ui.TexLODBias = settings->textureMIPBias.value_or(m_ui.TexLODBias);
    }

    if (m_cmdLine.stopAnimations)
        m_ui.EnableAnimations = false;

    m_progressLoading.Set(70);

    LocalConfig::PostSceneLoad( *this, m_ui );

    m_progressLoading.Set(90);

    if (m_materialsBaker!=nullptr) m_materialsBaker->SceneReloaded();
    if (m_envMapBaker!=nullptr) m_envMapBaker->SceneReloaded();
    if (m_lightsBaker!=nullptr) m_lightsBaker->SceneReloaded();
    if (m_ommBaker!=nullptr) m_ommBaker->SceneLoaded(*m_scene);

    m_progressLoading.Set(100);

    if (m_cmdLine.OverrideToRealtimeMode)
        m_ui.RealtimeMode = true;
    if (m_cmdLine.OverrideToReferenceMode)
        m_ui.RealtimeMode = false;
    
    if (m_cmdLine.OverrideAutoexposureOff)
    {
        m_ui.ToneMappingParams.autoExposure = false;
        m_ui.ToneMappingParams.exposureValue = 0.0f;
    }
    if (m_cmdLine.OverrideExposureOffset != FLT_MAX)
        m_ui.ToneMappingParams.exposureCompensation = m_cmdLine.OverrideExposureOffset;
    if (m_cmdLine.DisableFireflyFilters)
    {
        m_ui.RealtimeFireflyFilterEnabled = false;
        m_ui.ReferenceFireflyFilterEnabled = false;
    }
    if (m_cmdLine.DisablePostProcessFilters)
    {
        m_ui.EnableBloom = false;
    }
    if (m_cmdLine.cameraPosDirUp != "")
        SetCurrentCameraPosDirUp(m_cmdLine.cameraPosDirUp);

    m_ui.MaterialVariantIndex = 0;

    m_asyncLoadingInProgress = true;

#if RTXPT_WITH_PYTHON
    // Initialize the embedded Python interpreter (lazily) and queue the
    // command-line scripts/expressions so that they execute against a fully
    // populated scene.  Actual execution happens during Animate() below.
    if (m_pythonScripting && (!m_cmdLine.pythonScript.empty() || !m_cmdLine.pythonExpr.empty()))
    {
        if (m_pythonScripting->Initialize())
        {
            if (!m_cmdLine.pythonScript.empty())
                m_pythonScripting->QueueScriptFile(m_cmdLine.pythonScript);
            if (!m_cmdLine.pythonExpr.empty())
                m_pythonScripting->QueueScriptString(m_cmdLine.pythonExpr, "<--pythonExpr>");
        }
    }
#endif
}

bool Sample::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_zoomTool && m_zoomTool->KeyboardUpdate(key, scancode, action, mods))
        return true;

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_camera.KeyboardUpdate(key, scancode, action, mods);

    if (m_sampleGame && m_sampleGame->KeyboardUpdate(key, scancode, action, mods))
        return true;


    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS && mods != GLFW_MOD_CONTROL && mods != GLFW_MOD_ALT)
    {
        m_ui.EnableAnimations = !m_ui.EnableAnimations;
        return true;
    }
    if( key == GLFW_KEY_F2 && action == GLFW_PRESS )
        m_ui.ShowUI = !m_ui.ShowUI;
    if( key == GLFW_KEY_R && action == GLFW_PRESS && mods == GLFW_MOD_CONTROL )
        m_ui.ShaderReloadRequested = true;

#if DONUT_WITH_STREAMLINE
    if (key == GLFW_KEY_F13 && action == GLFW_PRESS)
    {
        // As GLFW abstracts away from Windows messages
        // We instead set the F13 as the PC_Ping key in the constants and compare against that.
         GetDeviceManager()->GetStreamline().ReflexTriggerPcPing(GetFrameIndex());
    }
#endif

    return true;
}

bool Sample::MousePosUpdate(double xpos, double ypos)
{
    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_camera.MousePosUpdate(xpos, ypos);
    if (m_sampleGame)   m_sampleGame->MousePosUpdate(xpos /** upscalingScale.x*/, ypos /** upscalingScale.y*/);

    float2 upscalingScale = float2(1,1);
    if (m_renderTargets != nullptr)
        upscalingScale = float2(m_renderSize)/float2(m_displaySize);

    m_pickPosition = uint2( static_cast<uint>( xpos * upscalingScale.x ), static_cast<uint>( ypos * upscalingScale.y ) );
    m_ui.MousePos = uint2( static_cast<uint>( xpos * upscalingScale.x ), static_cast<uint>( ypos * upscalingScale.y ) );

    if (m_zoomTool)     m_zoomTool->MousePosUpdate( xpos, ypos );

    return true;
}

bool Sample::MouseButtonUpdate(int button, int action, int mods)
{
    if (m_zoomTool)     
        if (m_zoomTool->MouseButtonUpdate(button, action, mods))
            return true;

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_camera.MouseButtonUpdate(button, action, mods);
    if (m_sampleGame)   m_sampleGame->MouseButtonUpdate(button, action, mods);

    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_2)
    {
        m_pick = true;
        m_pickInstance = true;
        m_ui.DebugPixel = m_pickPosition;
    }

#if DONUT_WITH_STREAMLINE
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    {
         GetDeviceManager()->GetStreamline().ReflexTriggerFlash(GetFrameIndex());
    }
#endif

    return true;
}

bool Sample::MouseScrollUpdate(double xoffset, double yoffset)
{
    if (!(m_sampleGame && m_sampleGame->CameraActive()))
    {
        //m_camera.MouseScrollUpdate(xoffset, yoffset);
        m_ui.CameraMoveSpeed *= 1.0f + yoffset*0.1f;
    }
    return true;
}

void Sample::Animate(float fElapsedTimeSeconds)
{
    if (m_ui.ActualFPSLimiter()>0)    // essential for stable video recording
        fElapsedTimeSeconds = 1.0f / (float)m_ui.ActualFPSLimiter();

    m_captureScriptManager->PreAnim(fElapsedTimeSeconds);

#if RTXPT_WITH_PYTHON
    // Drain any pending Python scripts. We do this on the renderer thread so
    // bindings observe a coherent scene state and so they can mutate UI
    // settings before rendering for the current frame happens.
    if (m_pythonScripting && IsSceneLoaded())
        m_pythonScripting->ProcessPendingScripts();
#endif

    m_lastDeltaTime = fElapsedTimeSeconds;

    m_camera.SetMoveSpeed(m_ui.CameraMoveSpeed);

    if( m_ui.ShaderAndACRefreshDelayedRequest > 0 )
    {
        m_ui.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (m_ui.ShaderAndACRefreshDelayedRequest <= 0 )
        {
            m_ui.ShaderAndACRefreshDelayedRequest = 0;
            m_ui.ShaderReloadRequested = true;
            m_ui.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = m_ui.EnableAnimations && m_ui.RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || m_ui.ResetAccumulation;

    if (m_sampleGame) m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);

    if (m_toneMappingPass)
        m_toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

    if (IsSceneLoaded() && enableAnimationUpdate)
    {
        if (enableAnimations)
            m_sceneTime += fElapsedTimeSeconds;
        if (m_sampleGame && m_sampleGame->IsInitialized())
            m_sceneTime = m_sampleGame->GetGameTime();

        for (const auto& anim : m_scene->GetSceneGraph()->GetAnimations())
        {
            double cutLeft = 0.0; double cutRight = 0.0;
            // if (anim->GetName() == "Take 001") // special hack for mesh drone anim - TODO: fix properly in future
            // { cutLeft = 0.333; cutRight = 0.0; }
            anim->Apply((float)fmod(m_sceneTime+cutLeft, anim->GetDuration()-cutLeft-cutRight));
        }
    }
    else
    {
        m_sceneTime = 0.0f;
    }

    m_selectedCameraIndex = std::min( m_selectedCameraIndex, GetSceneCameraCount()-1 );
    if (m_selectedCameraIndex > 0)
    {
        std::shared_ptr<donut::engine::PerspectiveCamera> sceneCamera = std::dynamic_pointer_cast<PerspectiveCamera>(m_scene->GetSceneGraph()->GetCameras()[m_selectedCameraIndex-1]);
        if (sceneCamera != nullptr)
            UpdateCameraFromScene( sceneCamera );
    }

    m_camera.Animate(fElapsedTimeSeconds);

    if (m_sampleGame) m_sampleGame->TickCamera(fElapsedTimeSeconds, m_camera);

    if (m_ui.CameraAntiRRSleepJitter>0)
    {
        float off = 0.05f * ((m_frameIndex%2)?(-m_ui.CameraAntiRRSleepJitter):(m_ui.CameraAntiRRSleepJitter));

        float3 dir = m_camera.GetDir();
        float3 right = normalize(cross(dir, m_camera.GetUp()));
        affine3 rot = rotation(right, off);
        dir = rot.transformVector(dir);

        m_camera.LookTo( m_camera.GetPosition(), dir, m_camera.GetUp() );
    }

    dm::float3 camPos = m_camera.GetPosition();
    dm::float3 camDir = m_camera.GetDir();
    dm::float3 camUp = m_camera.GetUp();

    // if camera moves, reset accumulation
    if (m_lastCamDir.x != camDir.x || m_lastCamDir.y != camDir.y || m_lastCamDir.z != camDir.z || m_lastCamPos.x != camPos.x || m_lastCamPos.y != camPos.y || m_lastCamPos.z != camPos.z
        || m_lastCamUp.x != camUp.x || m_lastCamUp.y != camUp.y || m_lastCamUp.z != camUp.z )
    {
        m_lastCamPos = camPos;
        m_lastCamDir = camDir;
        m_lastCamUp = camUp;
        if( !m_ui.RealtimeMode )
            m_ui.ResetAccumulation = true;
        m_gaussianSplatTemporalReset = true;
    }

    m_captureScriptManager->PostAnim();


    double frameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();
    if (frameTime > 0.0)
    {
#if DONUT_WITH_STREAMLINE
        if (m_ui.DLSSFGMultiplier != 1)
            m_fpsInfo = StringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G", frameTime * 1e3, m_ui.DLSSFGMultiplier, m_ui.DLSSFGMultiplier / frameTime);
        else
#endif
            m_fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);
    }

    // Window title
    std::string extraInfo = ", " + m_fpsInfo + ", " + m_currentSceneName + ", " + GetResolutionInfo() + ", (L: " + std::to_string(m_scene->GetSceneGraph()->GetLights().size()) + ", MAT: " + std::to_string(m_scene->GetSceneGraph()->GetMaterials().size())
        + ", MESH: " + std::to_string(m_scene->GetSceneGraph()->GetMeshes().size()) + ", I: " + std::to_string(m_scene->GetSceneGraph()->GetMeshInstances().size()) + ", SI: " + std::to_string(m_scene->GetSceneGraph()->GetSkinnedMeshInstances().size())
        //+ ", AvgLum: " + std::to_string((m_renderTargets!=nullptr)?(m_renderTargets->AvgLuminanceLastCaptured):(0.0f))
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";


    GetDeviceManager()->SetInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
}

std::string Sample::GetResolutionInfo() const
{
    if (m_renderTargets == nullptr || m_renderTargets->OutputColor == nullptr)
        return "uninitialized";

    if (dm::all(m_renderSize == m_displaySize))
        return std::to_string(m_renderSize.x) + "x" + std::to_string(m_renderSize.y);
    else
        return std::to_string(m_renderSize.x) + "x" + std::to_string(m_renderSize.y) + "->" + std::to_string(m_displaySize.x) + "x" + std::to_string(m_displaySize.y);
}

float Sample::GetAvgTimePerFrame() const
{
    if (m_benchFrames == 0)
        return 0.0f;
    std::chrono::duration<double> elapsed = (m_benchLast - m_benchStart);
    return float(elapsed.count() / m_benchFrames);
}

std::string Sample::GetCurrentCameraPosDirUp() const
{
    auto toString = [](const float3 & val)    { return std::to_string(val.x) + "," + std::to_string(val.y) + "," + std::to_string(val.z); };
    return toString(m_camera.GetPosition()) + "," + toString(m_camera.GetDir()) + "," + toString(m_camera.GetUp());
}

bool Sample::SetCurrentCameraPosDirUp(const std::string & val)
{
	if (val=="") 
        return false;
    bool ok = true;
    float3 worldPos, worldDir, worldUp;
    std::string temp = val;
    ok &= ParseFloat3Consume( temp, worldPos );
    ok &= ParseFloat3Consume( temp, worldDir );
    ok &= ParseFloat3Consume( temp, worldUp );
    if (ok)
        m_camera.LookAt( worldPos, worldPos + worldDir, worldUp );
    return ok;
}

void Sample::SetCameraVerticalFOV(float cameraFOV)
{
    m_cameraVerticalFOV = cameraFOV;
    m_cameraUseCustomIntrinsics = false;
    m_ui.ResetAccumulation = true;
    m_gaussianSplatTemporalReset = true;
}

void Sample::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (fx <= 0.0f || fy <= 0.0f || width <= 0.0f || height <= 0.0f)
        return;

    m_cameraIntrinsics = dm::float4(fx, fy, cx, cy);
    m_cameraIntrinsicsViewport = dm::float2(width, height);
    m_cameraVerticalFOV = 2.0f * std::atan(height / (2.0f * fy));
    m_cameraUseCustomIntrinsics = true;
    m_ui.ResetAccumulation = true;
    m_gaussianSplatTemporalReset = true;
}

void Sample::ClearCameraIntrinsics()
{
    m_cameraUseCustomIntrinsics = false;
    m_ui.ResetAccumulation = true;
    m_gaussianSplatTemporalReset = true;
}

void Sample::SaveCurrentCamera() const
{
    float3 worldPos = m_camera.GetPosition();
    float3 worldDir = m_camera.GetDir();
    float3 worldUp  = m_camera.GetUp();
    dm::dquat rotation;
    dm::affine3 sceneWorldToView = dm::scaling(dm::float3(1.f, 1.f, -1.f)) * dm::inverse(m_camera.GetWorldToViewMatrix()); // see SceneCamera::GetViewToWorldMatrix
    dm::decomposeAffine<double>( daffine3(sceneWorldToView), nullptr, &rotation, nullptr );

    float4x4 projMatrix = m_view->GetProjectionMatrix();
    bool rowMajor = true;
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    bool autoExposure = m_ui.ToneMappingParams.autoExposure;
    float exposureCompensation = m_ui.ToneMappingParams.exposureCompensation;
    float exposureValue = m_ui.ToneMappingParams.exposureValue;

    std::ofstream file;
    file.open(app::GetDirectoryWithExecutable( ) / "campos.txt", std::ios_base::out | std::ios_base::trunc );
    if( file.is_open() )
    {
        file << worldPos.x << " " << worldPos.y << " " << worldPos.z << " " << std::endl;
        file << worldDir.x << " " << worldDir.y << " " << worldDir.z << " " << std::endl;
        file << worldUp.x  << " " << worldUp.y  << " " << worldUp.z  << " " << std::endl;

        file << std::endl;
        file << "below is the camera node that can be included into the *.scene.json;" << std::endl;
        file << "'Cameras' node goes into 'Graph' array" << std::endl;
        file << std::endl;
        file << "{"                                                             << std::endl;
        file << "    \"name\": \"Cameras\","                                    << std::endl;
        file << "        \"children\" : ["                                      << std::endl;
        file << "    {"                                                         << std::endl;
        file << "        \"name\": \"Default\","                                   << std::endl;
        file << "        \"type\" : \"PerspectiveCameraEx\","                     << std::endl;
        file << "        \"translation\" : [" << std::to_string(worldPos.x) << ", " << std::to_string(worldPos.y) << ", " << std::to_string(worldPos.z) << "]," << std::endl;
        file << "        \"rotation\" : [" << std::to_string(rotation.x) << ", " << std::to_string(rotation.y) << ", " << std::to_string(rotation.z) << ", " << std::to_string(rotation.w) << "]," << std::endl;
        file << "        \"verticalFov\" : " << std::to_string(fovY)            << "," << std::endl;
        file << "        \"zNear\" : " << std::to_string(m_cameraZNear)         << "," << std::endl;
        file << "        \"enableAutoExposure\" : " << (autoExposure?"true":"false") << "," << std::endl;
        file << "        \"exposureCompensation\" : " << std::to_string(exposureCompensation) << "," << std::endl;
        file << "        \"exposureValue\" : " << std::to_string(exposureValue) << std::endl;
        file << "    }"                                                         << std::endl;
        file << "        ]"                                                     << std::endl;
        file << "},"                                                            << std::endl;

        file.close();
    }
}

void Sample::LoadCurrentCamera()
{
    float3 worldPos;
    float3 worldDir;
    float3 worldUp;

    std::ifstream file;
    file.open(app::GetDirectoryWithExecutable( ) / "campos.txt", std::ios_base::in);
    if (file.is_open())
    {
        file >> worldPos.x >> std::ws >> worldPos.y >> std::ws >> worldPos.z; file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file >> worldDir.x >> std::ws >> worldDir.y >> std::ws >> worldDir.z; file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file >> worldUp.x  >> std::ws >> worldUp.y  >> std::ws >> worldUp.z; file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file.close();
        m_camera.LookAt( worldPos, worldPos + worldDir, worldUp );
    }
}

void Sample::FillPTPipelineGlobalMacros(std::vector<donut::engine::ShaderMacro> & macros)
{
    macros.clear();

    auto device = GetDevice();
    const bool canUseNvapiHitObject =
        m_ui.NVAPIHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 &&
        device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV);
    const bool canUseDxHitObject =
        m_ui.DXHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;

    assert(!canUseNvapiHitObject || !canUseDxHitObject);

    macros.push_back({ "ENABLE_DEBUG_SURFACE_VIZ",  (m_ui.DebugView != DebugViewType::Disabled)?("1"):("0") });
    macros.push_back({ "ENABLE_DEBUG_LINES_VIZ",    (m_ui.ShowDebugLines)?("1"):("0") });

    macros.push_back({ "USE_NVAPI_HIT_OBJECT_EXTENSION", canUseNvapiHitObject ? "1" : "0" });
    macros.push_back({ "USE_NVAPI_REORDER_THREADS", (canUseNvapiHitObject && m_ui.NVAPIReorderThreads) ? "1" : "0" });

    macros.push_back({ "USE_DX_HIT_OBJECT_EXTENSION", canUseDxHitObject ? "1" : "0" });
    macros.push_back({ "USE_DX_MAYBE_REORDER_THREADS", (canUseDxHitObject && m_ui.DXMaybeReorderThreads) ? "1" : "0" });

    macros.push_back({ "PT_ENABLE_RUSSIAN_ROULETTE", (m_ui.EnableRussianRoulette) ? ("1") : ("0") });

    macros.push_back({ "PT_NEE_ENABLED", (m_ui.UseNEE)?("1"):("0") });

    macros.push_back({ "PT_USE_RESTIR_DI", (m_ui.ActualUseReSTIRDI()) ? ("1") : ("0") });   // these will match constants.useReSTIRDI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_GI", (m_ui.ActualUseReSTIRGI()) ? ("1") : ("0") });   // these will match constants.useReSTIRGI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_PT", (m_ui.ActualUseReSTIRPT()) ? ("1") : ("0") });
    

    // minor perf gains but recompile time every time value changed is too annoying 
    // macros.push_back({ "PT_BOUNCE_COUNT", std::to_string(m_ui.BounceCount) });
    // macros.push_back({ "PT_DIFFUSE_BOUNCE_COUNT", std::to_string((m_ui.RealtimeMode) ? (m_ui.RealtimeDiffuseBounceCount) : (m_ui.ReferenceDiffuseBounceCount)) });

    macros.push_back({ "RTXPT_USE_APPROXIMATE_MIS", (m_ui.ActualUseApproximateMIS()) ? ("1") : ("0") });

    // It helps performance a lot when these are baked in
    macros.push_back({ "RTXPT_NEE_FULL_SAMPLE_COUNT", std::to_string(m_ui.NEEFullSamples) });
    uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);
    uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_ui.ActualNEEAT_LocalToGlobalSampleRatio(), m_ui.NEECandidateSamples);
    macros.push_back({ "RTXPT_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidateSamples) });
    macros.push_back({ "RTXPT_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidateSamples) });
    macros.push_back({ "RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(m_ui.NEECandidateSamples) });

    macros.push_back({ "RTXPT_DISABLE_SER_TERMINATION_HINT", (m_ui.DbgDisableSERTerminationHint)?("1"):("0") });
    macros.push_back({ "RTXPT_DISCARD_NON_NEE_LIGHTING", (m_ui.DbgDiscardNonNEELighting) ? ("1") : ("0") });
    macros.push_back({ "RTXPT_DISCARD_NEE_LIGHTING", (m_ui.DbgDiscardNEELighting) ? ("1") : ("0") });
    
    macros.push_back({ "RTXPT_FIREFLY_FILTER", (m_ui.ActualFireflyFilterEnabled()) ? ("1") : ("0") });

    macros.push_back({ "RTXPT_ACTIVE_STABLE_PLANE_COUNT", std::to_string(m_ui.StablePlanesActiveCount) });

    macros.push_back({ "RTXPT_NESTED_DIELECTRICS_QUALITY", std::to_string(m_ui.NestedDielectricsQuality) });

    macros.push_back({ "RTXPT_LP_TYPES_USE_16BIT_PRECISION", (m_ui.UseFp16Types) ? ("1") : ("0") });

    macros.push_back({ "RTXPT_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", (m_ui.EnableLDSamplerForBSDF) ? ("1") : ("0") });

    m_lightsBaker->SetGlobalShaderMacros(macros);
    if (m_ommBaker != nullptr)
        m_ommBaker->SetGlobalShaderMacros(macros);
}

extern HitGroupInfo ComputeSubInstanceHitGroupInfo(const PTMaterial& material);

bool Sample::CreatePTPipeline(engine::ShaderFactory& shaderFactory)
{
    {
        std::vector<donut::engine::ShaderMacro> shaderMacros;
		// shaderMacros.push_back(donut::engine::ShaderMacro({ "USE_RTXDI", "0" }));
        m_exportVBufferCS = m_shaderFactory->CreateShader("app/ProcessingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
		pipelineDesc.CS = m_exportVBufferCS;
        m_exportVBufferPSO = GetDevice()->createComputePipeline(pipelineDesc);
    }

    return true;
}

void Sample::CreateBlases(nvrhi::ICommandList* commandList)
{
    for (const std::shared_ptr<MeshInfo>& mesh : m_scene->GetSceneGraph()->GetMeshes())
    {
        if (mesh->isSkinPrototype) //buffers->hasAttribute(engine::VertexAttribute::JointWeights))
            continue; // skip the skinning prototypes

        bvh::Config cfg = { .excludeTransmissive = m_ui.AS.ExcludeTransmissive };

        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg , *mesh, nullptr, false);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12)); // we can only hold 13 bits for the geometry index in the HitInfo - see GeometryInstanceID in SceneTypes.hlsli

        nvrhi::rt::AccelStructHandle as = GetDevice()->createAccelStruct(blasDesc);

        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);

        mesh->accelStruct = as;
    }
}

void Sample::UploadSubInstanceData(nvrhi::ICommandList* commandList)
{
    assert(m_subInstanceCount == m_subInstanceData.size());
    if (m_subInstanceData.empty())
        return;
    // upload data to GPU buffer
    commandList->writeBuffer(m_subInstanceBuffer, m_subInstanceData.data(), m_subInstanceData.size() * sizeof(SubInstanceData));
}

void Sample::CreateTlas(nvrhi::ICommandList* commandList)
{
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = std::max<size_t>(1, m_scene->GetSceneGraph()->GetMeshInstances().size());
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    assert( tlasDesc.topLevelMaxInstances < (1 << 15) ); // we can only hold 16 bits for the identifier in the HitInfo - see GeometryInstanceID in SceneTypes.hlsli
    m_topLevelAS = GetDevice()->createAccelStruct(tlasDesc);

    // setup subInstances (entry is per geometry per instance) - some of it might require rebuild at runtime in more realistic scenarios
    {
        // figure out the required number
        m_subInstanceCount = 0;
        for (const auto& instance : m_scene->GetSceneGraph()->GetMeshInstances())
            m_subInstanceCount += (uint)instance->GetMesh()->geometries.size();
        // create GPU buffer
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(SubInstanceData) * std::max(1u, m_subInstanceCount);
        bufferDesc.debugName = "Instances";
        bufferDesc.structStride = sizeof(SubInstanceData);
        bufferDesc.canHaveRawViews = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.isVertexBuffer = false;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;
        bufferDesc.keepInitialState = true;
        m_subInstanceBuffer = GetDevice()->createBuffer(bufferDesc);
        // figure out the data
        m_subInstanceData.clear();
        m_subInstanceData.insert(m_subInstanceData.begin(), m_subInstanceCount, SubInstanceData{ 0 } );
    }
}

void Sample::CreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if(m_ommBaker) m_ommBaker->CreateOpacityMicromaps(*m_scene);
    CreateBlases(commandList);
    CreateTlas(commandList);
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            object.pass->BuildAccelerationStructures(
                commandList,
                m_ui.GaussianSplatUseAABBs,
                m_ui.GaussianSplatUseTLASInstances,
                m_ui.GaussianSplatBlasCompaction,
                m_ui.GaussianSplatScale,
                uint32_t(std::clamp(m_ui.GaussianSplatRtxKernelDegree, 0, 5)),
                m_ui.GaussianSplatRtxAdaptiveClamp);
        else
            object.pass->ReleaseAccelerationStructures();
    }
}

void Sample::RecreateAccelStructs(nvrhi::ICommandList* commandList)
{
    // If the subInstanceData or BLAS build input data changes we trigger a full update here
    // could be made more efficient by only rebuilding the geometry in question,
    // or split the BLAS and subInstanceData updates
    if (m_ui.AccelerationStructRebuildRequested)
    {
        m_ui.AccelerationStructRebuildRequested = false;
        m_ui.ResetAccumulation = true;

        GetDevice()->waitForIdle();

        m_bindingSet = nullptr;
        m_topLevelAS = nullptr;

        for (const std::shared_ptr<MeshInfo>& _mesh : m_scene->GetSceneGraph()->GetMeshes())
        {
            assert(std::dynamic_pointer_cast<MeshInfoEx>(_mesh) != nullptr);
            const std::shared_ptr<MeshInfoEx>& mesh = std::static_pointer_cast<MeshInfoEx>(_mesh);
            mesh->accelStruct = nullptr;
            mesh->AccelStructOMM = nullptr;
            mesh->OpacityMicroMaps.clear();
            mesh->DebugData = nullptr;
            mesh->DebugDataDirty = true;
        }
        m_meshesPendingAccelRebuild.clear();
        GetDevice()->runGarbageCollection();

        // raytracing acceleration structures
        commandList->open();
        CreateAccelStructs(commandList);
        commandList->close();
        GetDevice()->executeCommandList(commandList);
        GetDevice()->waitForIdle();
    }
}

void Sample::RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    m_ui.ResetAccumulation = true;

    if (!m_topLevelAS)
    {
        m_ui.AccelerationStructRebuildRequested = true;
        return;
    }

    const auto it = std::find(m_meshesPendingAccelRebuild.begin(), m_meshesPendingAccelRebuild.end(), mesh);
    if (it == m_meshesPendingAccelRebuild.end())
        m_meshesPendingAccelRebuild.push_back(mesh);
}

void Sample::RebuildDirtyMeshAccelStructs(nvrhi::ICommandList* commandList)
{
    if (m_meshesPendingAccelRebuild.empty())
        return;

    std::vector<std::shared_ptr<MeshInfo>> dirtyMeshes;
    dirtyMeshes.swap(m_meshesPendingAccelRebuild);

    for (const std::shared_ptr<MeshInfo>& mesh : dirtyMeshes)
    {
        if (!mesh || mesh->isSkinPrototype)
            continue;

        if (!mesh->buffers || !mesh->buffers->vertexBuffer || !mesh->buffers->indexBuffer)
        {
            m_ui.AccelerationStructRebuildRequested = true;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = m_ui.AS.ExcludeTransmissive };
        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg, *mesh, nullptr, false);
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastBuild;
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));

        nvrhi::rt::AccelStructHandle as = GetDevice()->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
        mesh->accelStruct = as;

        if (auto meshEx = std::dynamic_pointer_cast<MeshInfoEx>(mesh))
        {
            meshEx->AccelStructOMM = nullptr;
            meshEx->OpacityMicroMaps.clear();
            meshEx->DebugData = nullptr;
            meshEx->DebugDataDirty = true;
        }
    }
}

void Sample::TransitionMeshBuffersToReadOnly(nvrhi::ICommandList* commandList)
{
    // Transition all the buffers to their necessary states before building the BLAS'es to allow BLAS batching
    for (const auto& skinnedInstance : m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
        commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
}

void Sample::UpdateSkinnedBLASs(nvrhi::ICommandList* commandList, uint32_t frameIndex) const
{
    commandList->beginMarker("Skinned BLAS Updates");

    // Transition all the buffers to their necessary states before building the BLAS'es to allow BLAS batching
    for (const auto& skinnedInstance : m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
    {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
            continue;

        commandList->setAccelStructState(skinnedInstance->GetMesh()->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    // Now update the BLAS'es
    for (const auto& skinnedInstance : m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
    {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
            continue;

        bvh::Config cfg = { .excludeTransmissive = m_ui.AS.ExcludeTransmissive };

        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg, *skinnedInstance->GetMesh(), nullptr, true);

        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, skinnedInstance->GetMesh()->accelStruct, blasDesc);
    }
    commandList->endMarker();
}

void Sample::BuildTLAS(nvrhi::ICommandList* commandList) const
{
    std::vector<nvrhi::rt::InstanceDesc> instances; // TODO: make this a member, avoid allocs :)

    uint subInstanceCount = 0;
    for (const auto& instance : m_scene->GetSceneGraph()->GetMeshInstances())
    {
        const bool ommDebugViewEnabled = m_ommBaker && m_ommBaker->UIData().DebugView != OpacityMicroMapDebugView::Disabled;
        // ommDebugViewEnabled must do two things: use a BLAS without OMMs and disable all alpha testing.
        // This may sound a bit counter intuitive, the goal is to intersect micro-triangles marked as transparent without them actually being treated as such.

        const std::shared_ptr<MeshInfoEx>& mesh = std::static_pointer_cast<MeshInfoEx>(instance->GetMesh());

        const bool forceOpaque          = ommDebugViewEnabled || m_ui.AS.ForceOpaque;
        const bool hasAttachementOMM    = m_ommBaker && mesh->AccelStructOMM.Get() != nullptr;
        const bool useOmmBLAS           = m_ommBaker && m_ommBaker->UIData().Enable && hasAttachementOMM && !forceOpaque;

        const uint meshSubInstanceCount = (uint)mesh->geometries.size();
        assert(subInstanceCount == instance->GetGeometryInstanceIndex());

        auto* bottomLevelAS = useOmmBLAS ? mesh->AccelStructOMM.Get() : mesh->accelStruct.Get();
        if (bottomLevelAS == nullptr)
        {
            static bool warnedNullBlas = false;
            if (!warnedNullBlas)
            {
                log::warning("BuildTLAS skipped one or more mesh instances with null BLAS to avoid invalid TLAS input.");
                warnedNullBlas = true;
            }
            subInstanceCount += meshSubInstanceCount;
            continue;
        }

        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = bottomLevelAS;
        instanceDesc.instanceMask = (m_ommBaker && m_ommBaker->UIData().OnlyOMMs && !hasAttachementOMM) ? 0 : 1;
        instanceDesc.instanceID = instance->GetGeometryInstanceIndex();
        instanceDesc.instanceContributionToHitGroupIndex = subInstanceCount;
        instanceDesc.flags = (m_ommBaker && m_ommBaker->UIData().Force2State) ? nvrhi::rt::InstanceFlags::ForceOMM2State : nvrhi::rt::InstanceFlags::None;
        if (forceOpaque)
            instanceDesc.flags = (nvrhi::rt::InstanceFlags)((uint32_t)instanceDesc.flags | (uint32_t)nvrhi::rt::InstanceFlags::ForceOpaque);

        subInstanceCount += meshSubInstanceCount;

        auto node = instance->GetNode();
        assert(node);
        dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), instanceDesc.transform);

        instances.push_back(instanceDesc);
    }
    assert (m_subInstanceCount == subInstanceCount);

    commandList->beginMarker("TLAS Update");
    commandList->buildTopLevelAccelStruct(
        m_topLevelAS,
        instances.empty() ? nullptr : instances.data(),
        instances.size(),
        nvrhi::rt::AccelStructBuildFlags::AllowEmptyInstances);
    commandList->endMarker();
}


void Sample::BackBufferResizing()
{
    ApplicationBase::BackBufferResizing();
    
    GetDevice()->waitForIdle();
    GetDevice()->runGarbageCollection();
    m_bindingCache->Clear();
    m_renderTargets = nullptr;
    m_linesPipeline = nullptr; // the pipeline is based on the framebuffer so needs a reset
    for (int i=0; i < std::size(m_nrd); i++ )
        m_nrd[i] = nullptr;
    if (m_rtxdiPass)
        m_rtxdiPass->Reset();

// NOTE: we're not yet sure if this is necessary to avoid crash with going in/out of fullscreen and FG
#if DONUT_WITH_STREAMLINE
    if (!GetDeviceManager()->GetDeviceParams().headlessDevice &&
        (m_ui.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_ui.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        GetDeviceManager()->GetStreamline().CleanupDLSS(false);
        GetDeviceManager()->GetStreamline().CleanupDLSSG(false);

        if (GetDeviceManager()->GetStreamline().IsDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            GetDeviceManager()->GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_ui.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_ui.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            GetDeviceManager()->GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_ui.DLSSFGOptions = dlssgOptions;
        }
    }
#endif
}

void Sample::CreateRenderPasses( bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList )
{
    m_bindingCache->Clear();

    const uint2 screenResolution = {m_renderTargets->OutputColor->getDesc().width, m_renderTargets->OutputColor->getDesc().height};

    m_shaderDebug = std::make_shared<ShaderDebug>(GetDevice(), initializeCommandList, m_shaderFactory, m_CommonPasses);

    if (m_ui.ActualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;

    m_accumulationPass = std::make_unique<AccumulationPass>(GetDevice(), m_shaderFactory);
    m_accumulationPass->CreatePipeline();
    m_accumulationPass->CreateBindingSet(m_renderTargets->OutputColor, m_renderTargets->AccumulatedRadiance, m_renderTargets->ProcessedOutputColor);

    {
        nvrhi::TextureDesc gaussianCurrentDesc = m_renderTargets->ProcessedOutputColor->getDesc();
        gaussianCurrentDesc.debugName = "GaussianSplatTemporalCurrentColor";
        gaussianCurrentDesc.isUAV = false;
        gaussianCurrentDesc.isRenderTarget = false;
        gaussianCurrentDesc.useClearValue = false;
        gaussianCurrentDesc.clearValue = nvrhi::Color(0.0f);
        gaussianCurrentDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        gaussianCurrentDesc.keepInitialState = true;
        m_gaussianSplatCurrentColor = GetDevice()->createTexture(gaussianCurrentDesc);

        nvrhi::TextureDesc gaussianAccumDesc = m_renderTargets->ProcessedOutputColor->getDesc();
        gaussianAccumDesc.debugName = "GaussianSplatTemporalAccumulatedColor";
        gaussianAccumDesc.format = nvrhi::Format::RGBA32_FLOAT;
        gaussianAccumDesc.isUAV = true;
        gaussianAccumDesc.isRenderTarget = true;
        gaussianAccumDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        gaussianAccumDesc.keepInitialState = true;
        m_gaussianSplatAccumulatedColor = GetDevice()->createTexture(gaussianAccumDesc);

        m_gaussianSplatAccumulationPass = std::make_unique<AccumulationPass>(GetDevice(), m_shaderFactory);
        m_gaussianSplatAccumulationPass->CreatePipeline();
        m_gaussianSplatAccumulationPass->CreateBindingSet(m_gaussianSplatCurrentColor, m_gaussianSplatAccumulatedColor, m_renderTargets->ProcessedOutputColor);
        m_gaussianSplatTemporalReset = true;
    }

    // these get re-created every time intentionally, to pick up changes after at-runtime shader recompile
    m_toneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->LdrFramebuffer, *m_view, m_renderTargets->OutputColor);
    m_bloomPass = std::make_unique<BloomPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->ProcessedOutputFramebuffer, *m_view);
    m_postProcess = std::make_shared<PostProcess>(GetDevice(), m_shaderFactory, m_CommonPasses, m_shaderDebug);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_renderTargets->Depth;
        taaParams.motionVectors = m_renderTargets->ScreenMotionVectors;
        taaParams.unresolvedColor = m_renderTargets->OutputColor;
        taaParams.resolvedColor = m_renderTargets->ProcessedOutputColor;
        taaParams.feedback1 = m_renderTargets->TemporalFeedback1;
        taaParams.feedback2 = m_renderTargets->TemporalFeedback2;
        taaParams.historyClampRelax = m_renderTargets->CombinedHistoryClampRelax;
        taaParams.motionVectorStencilMask = 0; ///*uint32_t motionVectorStencilMask =*/ 0x01;
        taaParams.useCatmullRomFilter = true;

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, *m_view, taaParams);
    }

    if (!CreatePTPipeline(*m_shaderFactory))
        { assert(false); }

    if (m_envMapBaker == nullptr)
        m_envMapBaker = std::make_shared<EnvMapBaker>(GetDevice(), m_TextureCache, NeedsRasterPrecompute());
    if (m_lightsBaker == nullptr)
        m_lightsBaker = std::make_shared<LightsBaker>(GetDevice());
    m_envMapBaker->CreateRenderPasses(m_shaderDebug, m_shaderFactory, m_computePipelineBaker);
    m_envMapBaker->GenerateBRDFLUT(initializeCommandList.Get(), *m_bindingCache);  // One-time BRDF LUT generation
    m_lightsBaker->CreateRenderPasses(m_shaderFactory, m_bindlessLayout, m_CommonPasses, m_shaderDebug, screenResolution, m_envMapBaker->GetImportanceSampling()->GetImportanceMapResolution());

    if (!m_gaussianSplatSceneObjects.empty())
    {
        for (auto& object : m_gaussianSplatSceneObjects)
        {
            if (object.pass != nullptr && object.pass->HasSplats())
                PrepareGaussianSplatPass(*object.pass);
        }
    }

    m_denoisingGuidesBaker = std::make_shared<DenoisingGuidesBaker>(GetDevice(), m_shaderFactory, m_renderTargets, m_shaderDebug, m_bindingLayout);
}

void Sample::SetEnvMapOverrideSource(const std::string& envMapOverride) 
{ 
    if (m_envMapOverride != envMapOverride && m_envMapBaker != nullptr)
        m_envMapBaker->SetTargetCubeResolution(0);  // reset resolution just to avoid getting crazy with procedural sky as it's very slow
    m_envMapOverride = envMapOverride; 
}


void Sample::PreUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    RAII_SCOPE(m_commandList->beginMarker("PreUpdateLighting"); , m_commandList->endMarker(); );

    auto preUpdateCube = m_envMapBaker->GetEnvMapCube();

    std::filesystem::path sceneDirectory;
    if (m_currentScenePath != std::filesystem::path(c_InlineSceneSentinel))
        sceneDirectory = m_currentScenePath.parent_path();

    std::string envMapActualPath = m_envMapLocalPath; 
    if (m_envMapOverride != "" && m_envMapOverride != c_EnvMapSceneDefault)
        envMapActualPath = (IsProceduralSky(m_envMapOverride.c_str()))?(m_envMapOverride):(std::string(c_EnvMapSubFolder) + "/" + m_envMapOverride);

    if (!envMapActualPath.empty() && !IsProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = ResolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    m_envMapBaker->PreUpdate(commandList, m_CommonPasses, envMapActualPath, sceneDirectory);

    if (preUpdateCube != m_envMapBaker->GetEnvMapCube())
        needNewBindings = true;
}

void Sample::UpdateLighting(nvrhi::CommandListHandle commandList)
{
    RAII_SCOPE( m_commandList->beginMarker("UpdateLighting");, m_commandList->endMarker(); );

    EMB_DirectionalLight dirLights[EnvMapBaker::c_MaxDirLights];
    uint dirLightCount = 0;
    {   // Find and pre-process directional analytic lights, and convert them to environment map local frame so they remain pointing in correct world direction!
        float3 rotationInRadians = radians(m_ui.EnvironmentMapParams.RotationXYZ);
        affine3 rotationTransform = donut::math::rotation(rotationInRadians);
        affine3 inverseTransform = inverse(rotationTransform);
        for (int i = 0; i < (int)m_lights.size(); i++)
        {
            std::shared_ptr<DirectionalLight> dirLight = std::dynamic_pointer_cast<DirectionalLight>(m_lights[i]);
            if( dirLight != nullptr )
            {
                LightConstants light;
                dirLight->FillLightConstants(light);

                const float minAngularSize = PI_f / (m_envMapBaker->GetTargetCubeResolution()/2.0f);
                assert( light.angularSizeOrInvRange >= minAngularSize );    // point lights smaller than this cannot be reliably baked into cubemap
                dirLights[dirLightCount].AngularSize = std::max( light.angularSizeOrInvRange, minAngularSize );
                dirLights[dirLightCount].ColorIntensity = float4(light.color, light.intensity);
                dirLights[dirLightCount].Direction = rotationTransform.transformVector(light.direction);
                dirLightCount++;
            }
        }
    }

    if (m_envMapBaker->Update(commandList, *m_bindingCache, m_CommonPasses, EnvMapBaker::BakeSettings { .EnvMapRadianceScale = c_envMapRadianceScale }, m_sceneTime, dirLights, dirLightCount, !m_ui.RealtimeMode || !m_ui.EnableAnimations) )
        m_ui.ResetAccumulation = true;

    {
        LightsBaker::BakeSettings settings;
        settings.ImportanceSamplingType = (uint)m_ui.NEEType;
        settings.CameraPosition = m_camera.GetPosition();
        settings.CameraDirection = m_camera.GetDir();
        settings.ViewProjMatrix = m_view->GetViewProjectionMatrix();
        settings.MouseCursorPos = m_ui.MousePos;
        settings.GlobalTemporalFeedbackWeight   = m_ui.NEEAT_GlobalTemporalFeedbackWeight;
        settings.LocalToGlobalSampleRatio       = m_ui.ActualNEEAT_LocalToGlobalSampleRatio();
        settings.UseApproximateMIS              = m_ui.ActualUseApproximateMIS();
        settings.DistantVsLocalImportanceScale  = m_ui.NEEAT_Distant_vs_Local_Importance;
        settings.ResetFeedback = m_ui.ResetAccumulation && !m_ui.RealtimeMode 
#if 1
            || m_ui.ResetRealtimeCaches
#endif
        ;
        settings.PrevViewportSize = float2( (float)m_viewPrevious->GetViewExtent().width(), (float)m_viewPrevious->GetViewExtent().height() );
        settings.ViewportSize = float2( (float)m_view->GetViewExtent().width(), (float)m_view->GetViewExtent().height() );
        settings.EnvMapParams = LightsBakerEnvMapParams{ .Transform = m_envMapSceneParams.Transform, .InvTransform = m_envMapSceneParams.InvTransform, .ColorMultiplier = m_envMapSceneParams.ColorMultiplier, .Enabled = m_envMapSceneParams.Enabled };
        settings.FrameIndex = m_frameIndex;

        BuildGaussianSplatEmissionProxyList();
        if (!m_gaussianSplatEmissionProxies.empty() && IsGaussianSplatEmissionEnabled(m_ui))
        {
            settings.GaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
            settings.GaussianSplatEmissionObjectToWorld = float4x4::identity();
            settings.GaussianSplatEmissionIntensity = m_ui.GaussianSplatEmissionIntensity;
        }

        m_lightsBaker->UpdateBegin(commandList, *m_bindingCache, settings, m_sceneTime, m_scene, m_materialsBaker, m_ommBaker, m_subInstanceBuffer, m_subInstanceData, m_envMapBaker->GetImportanceSampling()->GetRadianceAndImportanceMap());
    }
}

void Sample::PreUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_ui.RealtimeMode && (resetAccum || m_ui.ResetAccumulation || m_ui.ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_ui.ReferenceOIDNDenoiserChanged)
    {
        ResetReferenceOIDN();
        m_ui.ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_ui.ResetAccumulation;
    resetAccum |= m_ui.RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_ui.AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum)
        m_shaderDebug->ClearDebugVizTexture(commandList);
#endif

    // profile perf - only makes sense with high accumulation sample counts; only start counting after n-th after it stabilizes
    if( m_accumulationSampleIndex < 16 )
    {
        m_benchStart = std::chrono::high_resolution_clock::now( );
        m_benchLast = m_benchStart;
        m_benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_ui.AccumulationTarget )
    {
        m_benchFrames++;
        m_benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_ui.RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_ui.AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_ui.AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_ui.DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}

void Sample::PostUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_ui.AccumulationTarget );

    if (m_ui.ActualUseRTXDIPasses())
        m_rtxdiPass->EndFrame();

    m_ui.ResetAccumulation = false;
    m_ui.ResetRealtimeCaches = false;
    m_frameIndex++;
}

void Sample::UpdatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData )
{
#if RTXPT_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    auto GetStfMagnificationMethod = [](StfMagnificationMethod method)->int {
        switch (method)
        {
        case StfMagnificationMethod::Default:   return STF_MAGNIFICATION_METHOD_NONE;
        case StfMagnificationMethod::Quad2x2:   return STF_MAGNIFICATION_METHOD_2x2_QUAD;
        case StfMagnificationMethod::Fine2x2:   return STF_MAGNIFICATION_METHOD_2x2_FINE;
        case StfMagnificationMethod::FineTemporal2x2: return STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL;
        case StfMagnificationMethod::FineAlu3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_ALU;
        case StfMagnificationMethod::FineLut3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_LUT;
        case StfMagnificationMethod::Fine4x4:    return STF_MAGNIFICATION_METHOD_4x4_FINE;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };

    auto GetStfFilterMode = [](StfFilterMode mode)->int {
        switch (mode)
        {
        case StfFilterMode::Point:      return STF_FILTER_TYPE_POINT;
        case StfFilterMode::Linear:     return STF_FILTER_TYPE_LINEAR;
        case StfFilterMode::Cubic:      return STF_FILTER_TYPE_CUBIC;
        case StfFilterMode::Gaussian:   return STF_FILTER_TYPE_GAUSSIAN;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };
#endif // RTXPT_STOCHASTIC_TEXTURE_FILTERING_ENABLE

    constants.camera = cameraData;
    constants.prevCamera = cameraData;
    if (m_viewPrevious)
        constants.prevCamera.PosW = m_viewPrevious->GetInverseViewMatrix().m_translation;

    constants.bounceCount = m_ui.BounceCount;
    constants.diffuseBounceCount = m_ui.DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_ui.RealtimeMode == false && m_ui.AccumulationAA)?(1):( (m_ui.RealtimeMode && m_ui.RealtimeAA == 3)?(m_ui.DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -donut::math::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_ui.TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_ui.ActualSamplesPerPixel();

    //constants.subSampleCount = m_ui.ActualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_ui.ActualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x; assert( m_renderSize.x == m_renderTargets->OutputColor->getDesc().width );
    constants.imageHeight = m_renderSize.y; assert( m_renderSize.y == m_renderTargets->OutputColor->getDesc().height );

    // this is the dynamic luminance that when passed through current tonemapper with current exposure settings, produces the same 50% gray
    constants.preExposedGrayLuminance = m_ui.EnableToneMapping?(donut::math::luminance(m_toneMappingPass->GetPreExposedGray(0))):(1.0f);

    const float disabledFF = 0.0f;
    if (m_ui.RealtimeMode)
        constants.fireflyFilterThreshold = (m_ui.RealtimeFireflyFilterEnabled)?(m_ui.RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_ui.ReferenceFireflyFilterEnabled)?(m_ui.ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_ui.ActualUseReSTIRDI();
    constants.useReSTIRGI = m_ui.ActualUseReSTIRGI();
    constants.useReSTIRPT = m_ui.ActualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_ui.EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_ui.DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_ui.DLSSRRBrightnessClampK>0)?(m_ui.DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_ui.ActualUseStandaloneDenoiser() || m_ui.RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_ui.StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_ui.StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_ui.BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_ui.AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_ui.StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_ui.StablePlanesSuppressPrimaryIndirectSpecular?m_ui.StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_ui.StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //GetFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_ui.UseNEE;
    constants.NEEType                           = m_ui.NEEType;
    constants.NEECandidateSamples               = m_ui.NEECandidateSamples;
    constants.NEEFullSamples                    = m_ui.NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_ui.EnvironmentMapDiffuseSampleMIPLevel;

#if RTXPT_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_ui.STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_ui.STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_ui.STFFilterMode);
    constants.STFGaussianSigma                  = m_ui.STFGaussianSigma;
#endif
}


void Sample::RtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_ui.EnvironmentMapParams.Enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_camera.GetPosition();
	bridgeParameters.userSettings = m_ui.RTXDI;
    bridgeParameters.usingLightSampling = m_ui.ActualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_ui.ActualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    BuildGaussianSplatEmissionProxyList();
    if (!m_gaussianSplatEmissionProxies.empty() && IsGaussianSplatEmissionEnabled(m_ui))
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_ui.GaussianSplatEmissionIntensity;
    }

    if( m_ui.ResetRealtimeCaches )
        m_rtxdiPass->Reset();

	m_rtxdiPass->PrepareResources(m_commandList, *m_renderTargets, envMapPresent ? m_envMapBaker : nullptr, m_envMapSceneParams,
        m_scene, m_materialsBaker, m_ommBaker, m_subInstanceBuffer, bridgeParameters, m_bindingLayout, m_shaderDebug );
 }

bool Sample::ShouldRenderUnfocused()
{
    if (m_frameIndex < 16 || m_ui.ResetAccumulation || m_ui.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork() )
    {
        // Make sure we at least run one render frame to allow expensive resource creation to happen in background, and to allow at least somewhat decent convergence so when user alt-tabs they get a nice image
        return true;
    }

    if (m_ui.RenderWhenOutOfFocus)
    {
        return true;
    }

    // Let Reference mode accumulate all frames before pausing
    return (!m_ui.RealtimeMode && (m_accumulationSampleIndex < m_ui.AccumulationTarget));
}

void Sample::StreamlinePreRender()
{
#if DONUT_WITH_STREAMLINE
    if (GetDeviceManager()->GetDeviceParams().headlessDevice)
        return;

    // Setup Reflex
    {
        auto reflexConsts = donut::app::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (donut::app::StreamlineInterface::ReflexMode) m_ui.ActualReflexMode();
        reflexConsts.frameLimitUs = m_ui.ReflexCappedFps == 0 ? 0 : int(1000000. / m_ui.ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        GetDeviceManager()->GetStreamline().SetReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        donut::app::StreamlineInterface::ReflexState reflexState{};
        GetDeviceManager()->GetStreamline().GetReflexState(reflexState);
        if (m_ui.IsReflexSupported)
        {
            m_ui.IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_ui.IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

            auto report = reflexState.frameReport[63];
            if (reflexState.lowLatencyAvailable && report.gpuRenderEndTime != 0)
            {
                auto frameID = report.frameID;
                auto totalGameToRenderLatencyUs = report.gpuRenderEndTime - report.inputSampleTime;
                auto simDeltaUs = report.simEndTime - report.simStartTime;
                auto renderDeltaUs = report.renderSubmitEndTime - report.renderSubmitStartTime;
                auto presentDeltaUs = report.presentEndTime - report.presentStartTime;
                auto driverDeltaUs = report.driverEndTime - report.driverStartTime;
                auto osRenderQueueDeltaUs = report.osRenderQueueEndTime - report.osRenderQueueStartTime;
                auto gpuRenderDeltaUs = report.gpuRenderEndTime - report.gpuRenderStartTime;

                m_ui.ReflexStats = "frameID: " + std::to_string(frameID);
                m_ui.ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_ui.ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_ui.ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_ui.ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_ui.ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_ui.ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_ui.ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_ui.ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (m_ui.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn && m_ui.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOff) {
            GetDeviceManager()->GetStreamline().CleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = m_ui.ActualDLSSFGMode();
        dlssgOptions.numFramesToGenerate = m_ui.DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (GetDeviceManager()->GetStreamline().IsDLSSGAvailable())
        {
            StreamlineInterface::DLSSGState state;
            GetDeviceManager()->GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_ui.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_ui.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            GetDeviceManager()->GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_ui.DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_ui.RealtimeAA == 3 && !m_ui.IsDLSSRRSupported)
    {
        log::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_ui.RealtimeAA = 2;
    }
    if ( m_ui.RealtimeAA == 2 && !m_ui.IsDLSSSuported )
    {
        log::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_ui.RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_ui.RealtimeAA >= 2 && m_ui.RealtimeAA <= 3) && m_ui.DLSSLastRealtimeAA != m_ui.RealtimeAA;
    {
        // Reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_ui.DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_ui.DLSSLastMode = SampleUIData::DLSSModeDefault;
            m_ui.DLSSMode = SampleUIData::DLSSModeDefault;
            m_ui.DLSSLastDisplaySize = { 0,0 };
        }

        m_ui.DLSSLastRealtimeAA = m_ui.RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3) && m_ui.RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_ui.IsDLSSSuported)
            {
                dlssOptions.mode = m_ui.DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_ui.RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    GetDeviceManager()->GetStreamline().SetDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_ui.RealtimeAA = 1" - check for recent code changes.
            }

            if (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_ui.DLSSMode != m_ui.DLSSLastMode) || (m_displaySize.x != m_ui.DLSSLastDisplaySize.x) || (m_displaySize.y != m_ui.DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    GetDeviceManager()->GetStreamline().QueryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_ui.RealtimeAA = 0;
                        m_ui.DLSSMode = SampleUIData::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_ui.DLSSLastMode = m_ui.DLSSMode;
                        m_ui.DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_ui.RealtimeAA == 3) // DLSS-RR
            {
                StreamlineInterface::DLSSRROptions dlssRROptions = {};
                dlssRROptions.mode              	= dlssOptions.mode;
                dlssRROptions.outputWidth       	= dlssOptions.outputWidth;
                dlssRROptions.outputHeight      	= dlssOptions.outputHeight;
                dlssRROptions.sharpness         	= dlssOptions.sharpness;
                dlssRROptions.preExposure       	= dlssOptions.preExposure;
                dlssRROptions.exposureScale     	= dlssOptions.exposureScale;
                dlssRROptions.colorBuffersHDR   	= dlssOptions.colorBuffersHDR;
                dlssRROptions.indicatorInvertAxisX 	= dlssOptions.indicatorInvertAxisX;
                dlssRROptions.indicatorInvertAxisY 	= dlssOptions.indicatorInvertAxisY;
                dlssRROptions.normalRoughnessMode 	= StreamlineInterface::DLSSRRNormalRoughnessMode::ePacked;
                dlssRROptions.alphaUpscalingEnabled = false;
                dlssRROptions.preset                = m_ui.DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_ui.IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                GetDeviceManager()->GetStreamline().SetDLSSOptions(dlssOptions);
            }

            m_renderSize = m_displaySize;
        }
    }
#else
    const bool changeToDLSSMode = false;
#endif // #if DONUT_WITH_STREAMLINE
}

#if RTXPT_WITH_NATIVE_DLSS
void Sample::NativeDLSSPreRender()
{
    if (!m_ui.RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_ui.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_ui.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
    }

    if (m_ui.RealtimeAA == 3 && !m_ui.IsDLSSRRSupported)
    {
        log::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_ui.RealtimeAA = 2;
    }

    if (m_ui.RealtimeAA == 2 && !m_ui.IsDLSSSuported)
    {
        log::warning("Requested DLSS mode not available. Switching to TAA.");
        m_ui.RealtimeAA = 1;
    }

    const bool usingDLSS = (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_ui.DLSSLastRealtimeAA != m_ui.RealtimeAA;

    if (changeToDLSSMode || m_ui.DLSSMode == SI::DLSSMode::eOff)
    {
        m_ui.DLSSLastMode = SampleUIData::DLSSModeDefault;
        m_ui.DLSSMode = SampleUIData::DLSSModeDefault;
        m_ui.DLSSLastDisplaySize = { 0, 0 };
    }

    m_ui.DLSSLastRealtimeAA = m_ui.RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_ui.DLSSMode != m_ui.DLSSLastMode) ||
            (m_displaySize.x != m_ui.DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_ui.DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_ui.DLSSLastMode = m_ui.DLSSMode;
            m_ui.DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_ui.DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif

void Sample::SetSceneTime( double sceneTime ) 
{ 
    if (m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_sceneTime = sceneTime; 
}

double Sample::GetSceneTime()
{
    if (m_sampleGame->IsInitialized())
        return m_sampleGame->GetGameTime();
    return m_sceneTime;
}


void Sample::PreRender()
{
    // Limit FPS
    if (m_ui.ActualFPSLimiter() > 0)
        g_FPSLimiter.FramerateLimit(m_ui.ActualFPSLimiter());

    korgi::Update();

    m_captureScriptManager->PreRender();
}

SimpleViewConstants FromPlanarViewConstants(PlanarViewConstants & view)
{
    SimpleViewConstants ret;
    ret.matWorldToView          = view.matWorldToView;
    ret.matViewToClip           = view.matViewToClip;
    ret.matWorldToClipNoOffset  = view.matWorldToClipNoOffset;
    ret.matClipToWorldNoOffset  = view.matClipToWorldNoOffset;
    ret.matWorldToClip          = view.matWorldToClip;
    ret.clipToWindowBias        = view.clipToWindowBias;
    ret.clipToWindowScale       = view.clipToWindowScale;
    ret.viewportOrigin          = view.viewportOrigin;
    ret.viewportSize            = view.viewportSize;
    ret.viewportSizeInv         = view.viewportSizeInv;
    ret.pixelOffset             = view.pixelOffset;
    return ret;
}

void Sample::PostProcessPreToneMapping(nvrhi::ICommandList* commandList, const donut::engine::ICompositeView& compositeView)
{ // a.k.a. HDR post-process (e.g. bloom goes here)
    donut::engine::PlanarView fullscreenView = *m_view;
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    if (m_ui.EnableBloom && m_ui.BloomIntensity > 0.f && m_ui.BloomRadius > 0.f)
    {
        m_bloomPass->Render(m_commandList, m_renderTargets->ProcessedOutputFramebuffer, fullscreenView, m_renderTargets->ProcessedOutputColor, m_ui.BloomRadius, m_ui.BloomIntensity);
    }

    if (m_ui.PostProcessTestPassHDR)
    {
        m_commandList->beginMarker("TestRaygenPP_HDR");

        m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        nvrhi::rt::DispatchRaysArguments args;
        args.width = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineTestRaygenPPHDR->GetShaderTable();
        state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        m_commandList->dispatchRays(args);

        m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_commandList->endMarker();
    }
}

void Sample::PostProcessPostToneMapping(nvrhi::ICommandList* commandList, const donut::engine::ICompositeView& compositeView)
{ // a.k.a. LDR post-process (e.g. colour filters go here)
    if (m_ui.PostProcessEdgeDetection)
    {
        m_commandList->beginMarker("PPEdgeDetection");

        m_commandList->copyTexture(m_renderTargets->LdrColorScratch, nvrhi::TextureSlice(), m_renderTargets->LdrColor, nvrhi::TextureSlice());

        nvrhi::rt::DispatchRaysArguments args;
        args.width  = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineEdgeDetection->GetShaderTable();
        state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4( *reinterpret_cast<uint*>(&m_ui.PostProcessEdgeDetectionThreshold), 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        m_commandList->dispatchRays(args);

        m_commandList->setTextureState(m_renderTargets->LdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_commandList->endMarker();
    }
}

void Sample::RenderGaussianSplats(bool renderToOutputColor)
{
    if (!m_ui.EnableGaussianSplats || m_gaussianSplatSceneObjects.empty())
        return;

    const bool stochasticSplats = m_ui.EnableGaussianSplats && m_ui.GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_ui.ResetAccumulation || m_ui.ResetRealtimeCaches || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_ui);
    GaussianSplatRenderSettings settings;
    settings.enabled = m_ui.EnableGaussianSplats;
    settings.depthTest = m_ui.GaussianSplatDepthTest;
    settings.sortingMode = m_ui.GaussianSplatSortingMode == 1 ? GaussianSplatSortMode::StochasticSplats : GaussianSplatSortMode::GpuSort;
    settings.renderTarget = renderToOutputColor ? GaussianSplatRenderTarget::OutputColor : GaussianSplatRenderTarget::ProcessedOutputColor;
    settings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(dm::clamp(m_ui.GaussianSplatFrustumCulling, 0, 2));
    settings.projectionMethod = GaussianSplatProjectionMethod::Eigen;
    settings.shFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_ui.GaussianSplatSHFormat, 0, 2));
    settings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_ui.GaussianSplatRGBAFormat, 0, 2));
    settings.screenSizeCulling = m_ui.GaussianSplatScreenSizeCulling;
    settings.mipSplattingAntialiasing = m_ui.GaussianSplatMipAntialiasing;
    settings.useAABBs = m_ui.GaussianSplatUseAABBs;
    settings.useTLASInstances = m_ui.GaussianSplatUseTLASInstances;
    settings.blasCompaction = m_ui.GaussianSplatBlasCompaction;
    settings.splatScale = m_ui.GaussianSplatScale;
    settings.alphaScale = m_ui.GaussianSplatAlphaScale;
    settings.brightness = m_ui.GaussianSplatBrightness;
    settings.tintColor = m_ui.GaussianSplatTintColor;
    settings.alphaCullThreshold = m_ui.GaussianSplatAlphaCullThreshold;
    settings.shadowsEnabled = gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    settings.shadowMode = gaussianSplatShadowMode;
    settings.shadowStrength = m_ui.GaussianSplatShadowStrength;
    settings.shadowRayOffset = m_ui.GaussianSplatRtxParticleShadowOffset;
    settings.shadowSoftRadius = m_ui.GaussianSplatShadowSoftRadius;
    settings.shadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_ui.GaussianSplatShadowSoftSampleCount);
    settings.shadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
    settings.frustumDilation = m_ui.GaussianSplatFrustumDilation;
    settings.minPixelCoverage = m_ui.GaussianSplatMinPixelCoverage;
    if (stochasticSplats && m_ui.RealtimeMode)
        settings.stochasticFrameIndex = uint32_t(m_gaussianSplatTemporalSampleIndex);
    else
        settings.stochasticFrameIndex = uint32_t(m_sampleIndex >= 0
            ? uint32_t(m_sampleIndex)
            : uint32_t(m_frameIndex & 0xffffffffu));
    for (const auto& light : m_lights)
    {
        std::shared_ptr<DirectionalLight> dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
        if (dirLight != nullptr)
        {
            LightConstants lightConstants;
            dirLight->FillLightConstants(lightConstants);
            settings.shadowDirectionToLight = -lightConstants.direction;
            break;
        }
    }

    donut::engine::PlanarView splatView = *m_view;
    if (!renderToOutputColor)
    {
        splatView.SetViewport(nvrhi::Viewport(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.SetPixelOffset(dm::float2::zero());
    }
    splatView.UpdateCache();

    bool renderedAny = false;
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        settings.objectToWorld = GetGaussianSplatObjectToWorld(object);
        object.pass->Render(m_commandList, splatView, m_topLevelAS.Get(), *m_renderTargets, settings);
        renderedAny = true;
    }

    if (renderedAny && stochasticSplats && !renderToOutputColor)
        AccumulateGaussianSplats(splatView);
}

void Sample::AccumulateGaussianSplats(const donut::engine::IView& splatView)
{
    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_ui.ResetAccumulation || m_ui.ResetRealtimeCaches || m_gaussianSplatTemporalReset)
    {
        m_gaussianSplatTemporalSampleIndex = 0;
        m_gaussianSplatTemporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(m_gaussianSplatTemporalSampleIndex + 1);

    m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    m_commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    m_commandList->commitBarriers();
    m_commandList->copyTexture(m_gaussianSplatCurrentColor, nvrhi::TextureSlice(), m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice());

    m_commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commandList->setTextureState(m_gaussianSplatAccumulatedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    m_commandList->commitBarriers();

    m_gaussianSplatAccumulationPass->Render(m_commandList, splatView, splatView, accumulationWeight);

    m_gaussianSplatTemporalSampleIndex = std::min(m_gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
}

void Sample::Render(nvrhi::IFramebuffer* framebuffer)
{
    const auto& fbinfo = framebuffer->getFramebufferInfo();
    m_displaySize = m_renderSize = uint2(fbinfo.width, fbinfo.height);
    float lodBias = 0.f;

    if (m_scene == nullptr)
    {
        assert( false ); // TODO: handle separately, just display pink color
        return;
    }
    m_progressLoading.Stop();
    m_asyncLoadingInProgress = false; // can be changed back to 'true' during the frame below, for example by OMMs

    HandleDroppedFiles();

    PreRender();

    StreamlinePreRender();
#if RTXPT_WITH_NATIVE_DLSS
    NativeDLSSPreRender();
#endif

 
    m_displayAspectRatio = m_displaySize.x/(float)m_displaySize.y;

    if (m_view == nullptr)
    {
        m_view = std::make_shared<PlanarView>();
        m_viewPrevious = std::make_shared<PlanarView>();
        m_viewPrevious->SetViewport(nvrhi::Viewport(float(m_renderSize.x), float(m_renderSize.y)));
        m_view->SetViewport(nvrhi::Viewport(float(m_renderSize.x), float(m_renderSize.y)));
    }

    bool needNewPasses = false;
    if( m_renderTargets == nullptr || m_renderTargets->IsUpdateRequired( m_renderSize, m_displaySize ) )
    {
        GetDevice()->waitForIdle();
        GetDevice()->runGarbageCollection();
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
        m_renderTargets = nullptr;
        m_oidnDenoisedOutput = nullptr;
        ResetReferenceOIDN();
        m_bindingCache->Clear( );
        m_renderTargets = std::make_unique<RenderTargets>( );
        m_renderTargets->Init(GetDevice( ), m_renderSize, m_displaySize, true, true, c_SwapchainCount);

        needNewPasses = true;
        OnRenderTargetsRecreated();
    }

    // Environment map settings
    if (m_ui.EnvironmentMapParams.Enabled)
    {
        float intensity = m_ui.EnvironmentMapParams.Intensity / c_envMapRadianceScale;
        m_envMapSceneParams.ColorMultiplier = m_ui.EnvironmentMapParams.TintColor * intensity;

        float3 rotationInRadians = donut::math::radians(m_ui.EnvironmentMapParams.RotationXYZ);
        affine3 rotationTransform = donut::math::rotation(rotationInRadians);
        affine3 inverseTransform = inverse(rotationTransform);
        affineToColumnMajor(rotationTransform, m_envMapSceneParams.Transform);
        affineToColumnMajor(inverseTransform, m_envMapSceneParams.InvTransform);
        m_envMapSceneParams.Enabled = 1;
    }
    else
    {
        m_envMapSceneParams.ColorMultiplier = {0,0,0};
        m_envMapSceneParams.Enabled = 0;
    }

    if (m_ui.ShaderReloadRequested)
    {
        m_ui.ShaderReloadRequested = false;
        m_shaderFactory->ClearCache();
        needNewPasses = true;
    }

    bool exposureResetRequired = false;

    if (m_ui.NRDModeChanged) // if changing between ReLAX and ReBLUR
    {
        needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_ui.ActualUseStandaloneDenoiser()) // clean up the memory if not used
    {
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }

    // Acceleration structures need some material info, whilst other passes need acceleration structures, so first set up materials if needed
    if (needNewPasses)
    {
        m_progressInitializingRenderer.Start("Initializing renderer...");

        if (m_materialsBaker == nullptr)
        {
            m_materialsBaker = std::make_shared<MaterialsBaker>(GetMaterialSpecializationShader(), GetDevice(), m_TextureCache, m_shaderFactory);
            assert( m_ptPipelineBaker == nullptr ); // there should be no cases where materials baker is null but ptPipelineBaker isn't
            
            m_ptPipelineBaker = std::make_shared<PTPipelineBaker>(GetDevice(), m_materialsBaker, m_bindingLayout, m_bindlessLayout);
            
            // Additional paths to monitor for compute shader hot reload (besides the default Shaders path)
            std::vector<std::filesystem::path> additionalShaderPaths = {
                GetRuntimeDirectory().parent_path() / "caustica/Lighting"  // For shaders outside the main Shaders folder
            };
            m_computePipelineBaker = std::make_shared<ComputePipelineBaker>(GetDevice(), additionalShaderPaths);
            
            CreateRTPipelines();
        }

        m_materialsBaker->CreateRenderPassesAndLoadMaterials(m_bindlessLayout, m_CommonPasses, m_scene, m_currentScenePath, GetLocalPath(c_AssetsFolder));
        m_progressInitializingRenderer.Set(5);
        CollectUncompressedTextures();
        if(m_ommBaker) m_ommBaker->CreateRenderPasses(m_bindlessLayout, m_CommonPasses);
        m_progressInitializingRenderer.Set(20);

        if (m_zoomTool == nullptr)
            m_zoomTool = std::make_unique<ZoomTool>(GetDevice(), m_shaderFactory);
    }

    // Changes to material properties and settings can require a BLAS/TLAS or subInstanceBuffer rebuild (alpha tested/exclusion flags etc); otherwise this is a no-op.
    RecreateAccelStructs(m_commandList);

    if (m_ui.ActualUseRTXDIPasses() && m_rtxdiPass == nullptr )
        needNewPasses = true; // this will initialize rtxdi passes
    if (!m_ui.ActualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    // this will also create or update materials which can trigger the need to update acceleration structures
    if (needNewPasses)
    {
        m_progressInitializingRenderer.Set(40);
        GetDevice()->waitForIdle();    // some subsystems have resources that could still be in use and might be deleted - make sure that's safe
        m_commandList->open();
        CreateRenderPasses(exposureResetRequired, m_commandList);
        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);
        m_progressInitializingRenderer.Set(70);
    }

    // this is the point where main ray tracing pipelines will actually get compiled
    m_ptPipelineBaker->Update(m_scene, (unsigned int)m_subInstanceData.size(), [this](std::vector<donut::engine::ShaderMacro> & macros){ this->FillPTPipelineGlobalMacros(macros); }, needNewPasses);
    
    // Update compute shaders (compile if needed)
    if (m_computePipelineBaker)
        m_computePipelineBaker->Update(needNewPasses);
    
    m_progressInitializingRenderer.Set(90);

    m_commandList->open();

    bool needNewBindings = false;
    PathTracerCameraData cameraData;
    {
        // Update camera data used by the path tracer & other systems
        UpdateViews(framebuffer);
        {   // TODO: pull all this to BridgeCamera - sizeX and sizeY are already inputs so we just need to pass projMatrix
            nvrhi::Viewport viewport = m_view->GetViewport();
            float2 jitter = m_view->GetPixelOffset();
            float4x4 projMatrix = m_view->GetProjectionMatrix();
            float2 viewSize = { viewport.maxX - viewport.minX, viewport.maxY - viewport.minY };
            float outputAspectRatio = m_displayAspectRatio; //windowViewport.width() / windowViewport.height();    // render and display outputs might not match in case of lower DLSS/etc resolution rounding!
            bool rowMajor = true;
            float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
            float fovY = atanf(tanHalfFOVY) * 2.0f;
            cameraData = BridgeCamera(uint(viewSize.x), uint(viewSize.y), outputAspectRatio, m_camera.GetPosition(), m_camera.GetDir(), m_camera.GetUp(), fovY, m_cameraZNear, 1e7f, m_ui.CameraFocalDistance, m_ui.CameraAperture, jitter);
        }

        if (needNewPasses || needNewBindings || m_bindingSet == nullptr)
            m_shaderDebug->CreateRenderPasses(framebuffer, m_renderTargets->Depth);

        if (m_ui.EnableShaderDebug)
        {
            dm::float4x4 viewProj = m_view->GetViewProjectionMatrix();
            m_shaderDebug->BeginFrame(m_commandList, viewProj);
        }

        // NOTE: this refreshes geometry buffers and updates stuff needed by m_ommBaker and m_materialsBaker and others below!
        m_scene->Refresh(m_commandList, GetFrameIndex());

        if(m_ommBaker) m_ommBaker->BuildOpacityMicromaps(*m_commandList, *m_scene);
        RebuildDirtyMeshAccelStructs(m_commandList);
        UpdateSkinnedBLASs(m_commandList, GetFrameIndex());
        m_commandList->compactBottomLevelAccelStructs(); // Compact acceleration structures that are tagged for compaction and have finished executing the original build
        BuildTLAS(m_commandList);
        TransitionMeshBuffersToReadOnly(m_commandList);
        if (m_ommBaker) 
        {
            m_asyncLoadingInProgress |= m_ommBaker->Update(*m_commandList, *m_scene);
            m_asyncLoadingInProgress |= m_ommBaker->UIData().BuildsLeftInQueue > 0;
        }

        m_materialsBaker->Update(m_commandList, m_scene, m_subInstanceData);
        UploadSubInstanceData(m_commandList); // this is now partial subInstance data, but lights baker update requires it to find materials and create emissive triangle lights

        // Update input lighting, environment map, etc.
        PreUpdateLighting(m_commandList, needNewBindings);

        // Early init for RTXDI
        if (m_rtxdiPass != nullptr) 
        {
            if (needNewPasses || needNewBindings || m_bindingSet == nullptr)
                m_rtxdiPass->Reset();
            RtxdiSetupFrame(framebuffer, cameraData, m_renderSize);
        }
    }

	if( needNewPasses || needNewBindings || m_bindingSet == nullptr )
    {
        m_progressInitializingRenderer.Set(95);
        RAII_SCOPE( m_commandList->close(); GetDevice()->executeCommandList(m_commandList);, m_commandList->open(););

        RecreateBindingSet();

        m_progressInitializingRenderer.Set(100);

        {
            nvrhi::BindingSetDesc lineBindingSetDesc;
            lineBindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
                nvrhi::BindingSetItem::Texture_SRV(0, m_renderTargets->Depth)
            };
            m_linesBindingSet = GetDevice()->createBindingSet(lineBindingSetDesc, m_linesBindingLayout);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.VS = m_linesVertexShader;
            psoDesc.PS = m_linesPixelShader;
            psoDesc.inputLayout = m_linesInputLayout;
            psoDesc.bindingLayouts = { m_linesBindingLayout };
            psoDesc.primType = nvrhi::PrimitiveType::LineList;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.blendState.targets[0].enableBlend().setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::Zero).setDestBlendAlpha(nvrhi::BlendFactor::One);

            m_linesPipeline = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer);
        }
        m_progressInitializingRenderer.Stop();
    }

    m_toneMappingPass->PreRender(m_ui.ToneMappingParams);

    PreUpdatePathTracing(needNewPasses, m_commandList);

    // I suppose we need to clear depth for right-click picking at least
    m_renderTargets->Clear( m_commandList );

    SampleConstants & constants = m_currentConstants; memset(&constants, 0, sizeof(constants));
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) }; // accessible but unused in path tracing at the moment
    if( m_scene == nullptr )
    {
        m_commandList->clearTextureFloat( m_renderTargets->OutputColor, nvrhi::AllSubresources, nvrhi::Color( 1, 1, 0, 0 ) );
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }
    else
    {
        UpdatePathTracerConstants(constants.ptConsts, cameraData);
        constants.MaterialCount = m_materialsBaker->GetMaterialDataCount(); // m_scene->GetSceneGraph()->GetMaterials().size();
        const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_ui);
        const GaussianSplatSceneObject* primaryGaussianSplat = GetPrimaryGaussianSplatObject();
        GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianSplat != nullptr ? primaryGaussianSplat->pass.get() : nullptr;
        constants.GaussianSplatShadowCount = (m_ui.EnableGaussianSplats
                && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
                && primaryGaussianSplatPass != nullptr
                && primaryGaussianSplatPass->GetTopLevelAS() != nullptr)
            ? primaryGaussianSplatPass->GetSplatCount()
            : 0;
        constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0 ? 1u : 0u;
        constants.GaussianSplatShadowScale = m_ui.GaussianSplatScale;
        constants.GaussianSplatShadowAlphaThreshold = m_ui.GaussianSplatAlphaCullThreshold;
        constants.GaussianSplatShadowUseTLASInstances =
            (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetShadowUsesTLASInstances()) ? 1u : 0u;
        constants.GaussianSplatShadowPrimitiveCountPerSplat =
            primaryGaussianSplatPass != nullptr ? primaryGaussianSplatPass->GetShadowPrimitiveCountPerSplat() : 1u;
        constants.GaussianSplatShadowMode = constants.GaussianSplatShadowsEnabled != 0
            ? gaussianSplatShadowMode
            : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        constants.GaussianSplatShadowSoftRadius = m_ui.GaussianSplatShadowSoftRadius;
        constants.GaussianSplatShadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_ui.GaussianSplatShadowSoftSampleCount);
        constants.GaussianSplatShadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
        constants.GaussianSplatShadowRayOffset = m_ui.GaussianSplatRtxParticleShadowOffset;
        constants.GaussianSplatShadowAlphaScale = m_ui.GaussianSplatAlphaScale;
        constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatKernelMinResponse;
        constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(m_ui.GaussianSplatRtxKernelDegree, 0, 5));
        constants.GaussianSplatShadowAdaptiveClamp = m_ui.GaussianSplatRtxAdaptiveClamp ? 1u : 0u;
        constants.GaussianSplatShadowWorldToObject = primaryGaussianSplat != nullptr
            ? inverse(GetGaussianSplatObjectToWorld(*primaryGaussianSplat))
            : float4x4::identity();

        constants.envMapSceneParams = m_envMapSceneParams;
        constants.envMapImportanceSamplingParams = m_envMapBaker->GetImportanceSampling()->GetShaderParams();

        PlanarViewConstants view;           m_view->FillPlanarViewConstants(view);
        PlanarViewConstants previousView;   m_viewPrevious->FillPlanarViewConstants(previousView);
        constants.view          = FromPlanarViewConstants(view);
        constants.previousView  = FromPlanarViewConstants(previousView);

        constants.debug = {};
        constants.debug.pick = m_pick || m_pickInstance || m_ui.ContinuousDebugFeedback;
        constants.debug.pickX = (constants.debug.pick)?(m_ui.DebugPixel.x):(-1);
        constants.debug.pickY = (constants.debug.pick)?(m_ui.DebugPixel.y):(-1);
        constants.debug.debugLineScale = (m_ui.ShowDebugLines)?(m_ui.DebugLineScale):(0.0f);
        constants.debug.showWireframe = m_ui.ShowWireframe;
        constants.debug.debugViewType = (int)m_ui.DebugView;
        constants.debug.debugViewStablePlaneIndex = (m_ui.StablePlanesActiveCount==1)?(0):(m_ui.DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
        constants.debug.exploreDeltaTree = (m_ui.ShowDeltaTree && constants.debug.pick)?(1):(0);
#else
        constants.debug.exploreDeltaTree = false;
#endif
        constants.debug.imageWidth = constants.ptConsts.imageWidth;
        constants.debug.imageHeight = constants.ptConsts.imageHeight;
        constants.debug.mouseX = m_ui.MousePos.x;
        constants.debug.mouseY = m_ui.MousePos.y;
        constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
        constants.debug._padding0 = 0;

        constants.denoisingHitParamConsts = { m_ui.ReblurSettings.hitDistanceParameters.A, m_ui.ReblurSettings.hitDistanceParameters.B,
                                              m_ui.ReblurSettings.hitDistanceParameters.C, m_ui.ReblurSettings.hitDistanceParameters.D };

        // This updates all lighting: distant (environment maps and directional analytic lights) and local (analytic lights and emissive triangle lights)
        // Must go before m_constantBuffer as when saving screenshots it closes and re-opens command list, flushing the volatile constant buffer!
        UpdateLighting(m_commandList);
        UploadSubInstanceData(m_commandList); // this is now full subInstance data

        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        SampleRenderCode(framebuffer, m_commandList, constants);

        const bool stochasticSplats = m_ui.EnableGaussianSplats && m_ui.GaussianSplatSortingMode == 1;
        const bool stochasticUsesMainTemporal = stochasticSplats && (!m_ui.RealtimeMode || m_ui.RealtimeAA == 1);
        if (stochasticUsesMainTemporal)
            RenderGaussianSplats(true);

        PostProcessAA(framebuffer, needNewPasses || m_ui.ResetRealtimeCaches);
        ApplyReferenceOIDN();
        if (m_ui.ReferenceOIDNDenoiser)
            m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        if (!stochasticUsesMainTemporal)
            RenderGaussianSplats(false);
    }

    donut::engine::PlanarView fullscreenView = *m_view;
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    PostProcessPreToneMapping(m_commandList, fullscreenView);   // writing to m_renderTargets->ProcessedOutputColor

    //Tone Mapping; it will read from m_renderTargets->ProcessedOutputColor and write into m_renderTargets->LdrColor; in case tonemapping is disabled, it's just a passthrough
    if (m_toneMappingPass->Render(m_commandList, fullscreenView, m_renderTargets->ProcessedOutputColor, m_ui.EnableToneMapping))
    {
        // first run tonemapper can close & re-open command list - when that happens, we have to re-upload volatile constants
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }

    PostProcessPostToneMapping(m_commandList, fullscreenView);  // writing to m_renderTargets->LdrColor

    //m_postProcess->Render(m_commandList, m_renderTargets->LdrColor);

    if (m_ui.EnableShaderDebug)
        m_shaderDebug->EndFrameAndOutput(m_commandList, m_renderTargets->LdrFramebuffer->GetFramebuffer(fullscreenView), m_renderTargets->Depth, fbinfo.getViewport());

    m_zoomTool->Render(m_commandList, m_renderTargets->LdrColor);

    m_commandList->beginMarker("Blit");
    m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->LdrColor, m_bindingCache.get());
    m_commandList->endMarker();

    if (m_ui.ShowDebugLines == true)
    {
        m_commandList->beginMarker("Debug Lines");

        // this draws the debug lines - should be the only actual rasterization around :)
        {
            nvrhi::GraphicsState state;
            state.bindings = { m_linesBindingSet };
            state.vertexBuffers = { {m_debugLineBufferDisplay, 0, 0} };
            state.pipeline = m_linesPipeline;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(fbinfo.getViewport());

            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = m_feedbackData.lineVertexCount;
            m_commandList->draw(args);
        }

        if (m_cpuSideDebugLines.size() > 0)
        {
            // using m_debugLineBufferCapture for direct drawing here
            m_commandList->writeBuffer( m_debugLineBufferCapture, m_cpuSideDebugLines.data(), sizeof(DebugLineStruct) * m_cpuSideDebugLines.size() );

            nvrhi::GraphicsState state;
            state.bindings = { m_linesBindingSet };
            state.vertexBuffers = { {m_debugLineBufferCapture, 0, 0} };
            state.pipeline = m_linesPipeline;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(fbinfo.getViewport());

            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = (uint32_t)m_cpuSideDebugLines.size();
            m_commandList->draw(args);
        }

        m_commandList->endMarker();
    }
    m_cpuSideDebugLines.clear();

    if( m_ui.ContinuousDebugFeedback || m_pick )
    {
        m_commandList->copyBuffer(m_feedback_Buffer_Cpu, 0, m_feedback_Buffer_Gpu, 0, sizeof(DebugFeedbackStruct) * 1);
        m_commandList->copyBuffer(m_debugLineBufferDisplay, 0, m_debugLineBufferCapture, 0, sizeof(DebugLineStruct) * MAX_DEBUG_LINES );
        m_commandList->copyBuffer(m_debugDeltaPathTree_Cpu, 0, m_debugDeltaPathTree_Gpu, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
	}

    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;


	m_commandList->close();
	GetDevice()->executeCommandList(m_commandList);

    // resolve picking and debug info
    if (m_ui.ContinuousDebugFeedback || m_pick || m_pickInstance)
    {
        GetDevice()->waitForIdle();
        void* pData = GetDevice()->mapBuffer(m_feedback_Buffer_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_feedbackData, pData, sizeof(DebugFeedbackStruct)* 1);
        GetDevice()->unmapBuffer(m_feedback_Buffer_Cpu);

        pData = GetDevice()->mapBuffer(m_debugDeltaPathTree_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_debugDeltaPathTree, pData, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
        GetDevice()->unmapBuffer(m_debugDeltaPathTree_Cpu);

        if (m_pick)
            m_ui.SelectedMaterial = FindMaterial(int(m_feedbackData.pickedMaterialID));

        if (m_pickInstance)
        {
            m_ui.SelectedNode = FindNodeByInstanceIndex(int(m_feedbackData.pickedInstanceIndex));
            if (m_ui.SelectedNode != nullptr)
                m_ui.SelectedGaussianSplat = false;
        }

        m_pick = false;
        m_pickInstance = false;
    }

    auto DumpScreenshot = [this, framebufferTexture](const char* fileName)
    {
        return SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, fileName);
    };

    m_captureScriptManager->PostRender(DumpScreenshot);

    if (m_ui.ExperimentalPhotoModeScreenshot)
    {
        DenoisedScreenshot( framebufferTexture );
        m_ui.ExperimentalPhotoModeScreenshot = false;
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->AdvanceFrame();

	std::swap(m_view, m_viewPrevious);
	GetDeviceManager()->SetVsyncEnabled(m_ui.ActualEnableVsync());

    PostUpdatePathTracing();
}

void Sample::RecreateBindingSet()
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    nvrhi::rt::IAccelStruct* gaussianSplatAS = m_topLevelAS;
    nvrhi::IBuffer* gaussianSplatBuffer = m_materialsBaker->GetMaterialDataBuffer();
    const GaussianSplatSceneObject* primaryGaussianSplat = GetPrimaryGaussianSplatObject();
    const GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianSplat != nullptr ? primaryGaussianSplat->pass.get() : nullptr;
    if (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetTopLevelAS() != nullptr && primaryGaussianSplatPass->GetSplatBuffer() != nullptr)
    {
        gaussianSplatAS = primaryGaussianSplatPass->GetTopLevelAS();
        gaussianSplatBuffer = primaryGaussianSplatPass->GetSplatBuffer();
    }

    // Fixed resources that do not change between binding sets
    nvrhi::BindingSetDesc bindingSetDescBase;
    bindingSetDescBase.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::PushConstants(1, sizeof(SampleMiniConstants)),
        //nvrhi::BindingSetItem::ConstantBuffer(2, m_lightsBaker->GetLightingConstants()),
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_topLevelAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_subInstanceBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_scene->GetInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_scene->GetGeometryBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, (m_ommBaker)?(m_ommBaker->GetGeometryDebugBuffer()):(m_materialsBaker->GetMaterialDataBuffer().Get()) ),   // YUCK
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_materialsBaker->GetMaterialDataBuffer()),
        nvrhi::BindingSetItem::Texture_SRV(6,  m_renderTargets->LdrColorScratch, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::RayTracingAccelStruct(7, gaussianSplatAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, gaussianSplatBuffer),
        nvrhi::BindingSetItem::Texture_SRV(10, m_envMapBaker->GetEnvMapCube()), //m_EnvironmentMap->IsEnvMapLoaded() ? m_EnvironmentMap->GetEnvironmentMap() : m_CommonPasses->m_BlackTexture),
        nvrhi::BindingSetItem::Texture_SRV(11, m_envMapBaker->GetImportanceSampling()->GetImportanceMapOnly()), //m_EnvironmentMap->IsImportanceMapLoaded() ? m_EnvironmentMap->GetImportanceMap() : m_CommonPasses->m_BlackTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_lightsBaker->GetControlBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_lightsBaker->GetLightBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(14, m_lightsBaker->GetLightExBuffer()),
        nvrhi::BindingSetItem::TypedBuffer_SRV(15, m_lightsBaker->GetLightProxyCounters()),     // t_tightProxyCounters
        nvrhi::BindingSetItem::TypedBuffer_SRV(16, m_lightsBaker->GetLightSamplingProxies()),   // t_LightProxyIndices
        nvrhi::BindingSetItem::TypedBuffer_SRV(17, m_lightsBaker->GetLocalSamplingBuffer()),    // t_LightLocalSamplingBuffer
        nvrhi::BindingSetItem::Texture_SRV(18, m_lightsBaker->GetEnvLightLookupMap()),          // t_EnvLookupMap
        //nvrhi::BindingSetItem::TypedBuffer_SRV(19, ),
        nvrhi::BindingSetItem::Texture_UAV(20, m_lightsBaker->GetFeedbackTotalWeight()),        // u_LightFeedbackTotalWeight
        nvrhi::BindingSetItem::Texture_UAV(21, m_lightsBaker->GetFeedbackCandidates()),         // u_LightFeedbackCandidates
        nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_AnisotropicWrapSampler),
        nvrhi::BindingSetItem::Sampler(1, m_envMapBaker->GetEnvMapCubeSampler()),
        nvrhi::BindingSetItem::Sampler(2, m_envMapBaker->GetImportanceSampling()->GetImportanceMapSampler()),
        nvrhi::BindingSetItem::Texture_UAV(0, m_renderTargets->OutputColor),
        nvrhi::BindingSetItem::Texture_UAV(1, m_renderTargets->ProcessedOutputColor),
        nvrhi::BindingSetItem::Texture_UAV(2, m_renderTargets->LdrColor, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_UAV(4, m_renderTargets->Throughput),
        nvrhi::BindingSetItem::Texture_UAV(5, m_renderTargets->ScreenMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(6, m_renderTargets->Depth),
        nvrhi::BindingSetItem::Texture_UAV(7, m_renderTargets->SpecularHitT), 
        nvrhi::BindingSetItem::Texture_UAV(8, m_renderTargets->ScratchFloat1), 
        nvrhi::BindingSetItem::Texture_UAV(31, m_renderTargets->DenoiserViewspaceZ),
        nvrhi::BindingSetItem::Texture_UAV(32, m_renderTargets->DenoiserMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(33, m_renderTargets->DenoiserNormalRoughness),
        nvrhi::BindingSetItem::Texture_UAV(34, m_renderTargets->DenoiserDiffRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(35, m_renderTargets->DenoiserSpecRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(36, m_renderTargets->DenoiserDisocclusionThresholdMix),
        nvrhi::BindingSetItem::Texture_UAV(37, m_renderTargets->CombinedHistoryClampRelax),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(51, m_feedback_Buffer_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(52, m_debugLineBufferCapture),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(53, m_debugDeltaPathTree_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(54, m_debugDeltaPathTreeSearchStack),
        nvrhi::BindingSetItem::Texture_UAV(60, m_renderTargets->SecondarySurfacePositionNormal),
        nvrhi::BindingSetItem::Texture_UAV(61, m_renderTargets->SecondarySurfaceRadiance),
        nvrhi::BindingSetItem::Texture_UAV(70, m_renderTargets->RRDiffuseAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(71, m_renderTargets->RRSpecAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(72, m_renderTargets->RRNormalsAndRoughness),
        nvrhi::BindingSetItem::Texture_UAV(73, m_renderTargets->RRSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(74, (m_renderTargets->RRTransparencyLayer!=nullptr)?m_renderTargets->RRTransparencyLayer:m_renderTargets->RRSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(75, m_renderTargets->DenoiserAvgLayerRadianceHalfRes),

        ///***
        nvrhi::BindingSetItem::Texture_UAV(100, m_renderTargets->BaseColor),
        nvrhi::BindingSetItem::Texture_UAV(101, m_renderTargets->SpecNormal),
        nvrhi::BindingSetItem::Texture_UAV(102, m_renderTargets->RoughnessMetal),
        nvrhi::BindingSetItem::Texture_UAV(103, m_renderTargets->MaterialInfo),
        nvrhi::BindingSetItem::Texture_UAV(10, m_renderTargets->LocalCubemap),  // u_LocalCubemap for RT pass
        ///***

        nvrhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->GetGPUWriteBuffer()),
        nvrhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->GetDebugVizTexture())
    };

    // NV HLSL extensions - DX12 only - we should probably expose some form of GetNvapiIsInitialized instead
    if (GetDevice()->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV))
    {
        bindingSetDescBase.bindings.push_back(
            nvrhi::BindingSetItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM, nullptr));
    }

    // Main raytracing & etc binding set
    {
        nvrhi::BindingSetDesc bindingSetDesc;

        bindingSetDesc.bindings = bindingSetDescBase.bindings;

        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(40, m_renderTargets->StablePlanesHeader));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(42, m_renderTargets->StablePlanesBuffer));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(44, m_renderTargets->StableRadiance));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(45, m_renderTargets->SurfaceDataBuffer));

        // Reflection system bindings (t80-t83, b3)
        // Derived classes can override AddCustomBindings to provide actual textures
        // Default to black texture fallbacks (NVRHI doesn't allow null textures)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(80, m_CommonPasses->m_BlackCubeMapArray));  // t_LocalCubemapGGX
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(81, m_CommonPasses->m_BlackCubeMapArray));  // t_DiffuseIrradianceCube
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(82, m_CommonPasses->m_BlackTexture));  // t_SSRBlurChain
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(83, (m_envMapBaker->GetBRDFLUT()!=nullptr)?m_envMapBaker->GetBRDFLUT():m_CommonPasses->m_BlackTexture ));  // t_BRDFLUT
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(84, m_CommonPasses->m_BlackTexture));  // t_DepthHierarchy placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(10, m_constantBuffer)); // ReflectionConstants (reuse main constant buffer as placeholder)
        
        // SSR result UAV placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(85, m_renderTargets->Depth));   // u_SSRResult placeholder

        // GTAO output (default to white = no occlusion; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(86, m_CommonPasses->m_WhiteTexture));  // t_GTAOOutput placeholder
        // Previous frame depth (default to black = zero depth; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(87, m_CommonPasses->m_BlackTexture));  // t_PrevDepth placeholder

        // Allow derived classes to customize bindings (e.g., add reflection textures, GTAO output)
        AddCustomBindings(bindingSetDesc);

        m_bindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_bindingLayout);
    }
}

std::shared_ptr<donut::engine::Material> Sample::FindMaterial(int materialID) const
{
    // if slow switch to map
    for (const auto& material : m_scene->GetSceneGraph()->GetMaterials())
        if (material->materialID == materialID)
            return material;
    return nullptr;
}

std::shared_ptr<donut::engine::SceneGraphNode> Sample::FindNodeByInstanceIndex(int instanceIndex) const
{
    if (!m_scene || instanceIndex < 0)
        return nullptr;

    const auto& instances = m_scene->GetSceneGraph()->GetMeshInstances();
    if (instanceIndex >= static_cast<int>(instances.size()))
        return nullptr;

    return instances[instanceIndex]->GetNodeSharedPtr();
}

namespace
{
    std::string TrimString(const std::string& text)
    {
        const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
        const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        return begin < end ? std::string(begin, end) : std::string();
    }

    std::vector<std::string> SplitWhitespace(const std::string& text)
    {
        std::vector<std::string> result;
        std::istringstream stream(text);
        std::string token;
        while (stream >> token)
            result.push_back(token);
        return result;
    }

    std::string ToLowerString(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string StripMatchingQuotes(const std::string& text)
    {
        if (text.size() >= 2)
        {
            const char first = text.front();
            const char last = text.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                return text.substr(1, text.size() - 2);
        }
        return text;
    }

    bool IsFloatToken(const std::string& text)
    {
        if (text.empty())
            return false;

        char* end = nullptr;
        std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0';
    }

    std::string JoinTokens(const std::vector<std::string>& tokens, size_t begin)
    {
        std::string result;
        for (size_t i = begin; i < tokens.size(); ++i)
        {
            if (!result.empty())
                result += ' ';
            result += tokens[i];
        }
        return result;
    }

    enum class ObjTextureChannel : uint8_t
    {
        Red,
        Green,
        Blue,
        Alpha,
        Luminance
    };

    struct ObjTextureReference
    {
        std::filesystem::path path;
        ObjTextureChannel channel = ObjTextureChannel::Red;
        float bumpMultiplier = 1.0f;
        bool hasBumpMultiplier = false;

        [[nodiscard]] bool empty() const { return path.empty(); }
    };

    ObjTextureChannel ParseObjTextureChannel(const std::string& text)
    {
        const std::string channel = ToLowerString(text);
        if (channel == "g" || channel == "green")
            return ObjTextureChannel::Green;
        if (channel == "b" || channel == "blue")
            return ObjTextureChannel::Blue;
        if (channel == "a" || channel == "alpha" || channel == "m" || channel == "matte")
            return ObjTextureChannel::Alpha;
        if (channel == "l" || channel == "lum" || channel == "luminance" || channel == "z")
            return ObjTextureChannel::Luminance;
        return ObjTextureChannel::Red;
    }

    ObjTextureReference ExtractObjTextureReference(const std::vector<std::string>& tokens)
    {
        ObjTextureReference result;
        size_t i = 1;
        while (i < tokens.size())
        {
            const std::string option = ToLowerString(tokens[i]);
            if (option.empty() || option[0] != '-' || IsFloatToken(option))
            {
                result.path = StripMatchingQuotes(JoinTokens(tokens, i));
                return result;
            }

            if (option == "-o" || option == "-s" || option == "-t")
            {
                ++i;
                for (int skipped = 0; i < tokens.size() && skipped < 3 && IsFloatToken(tokens[i]); ++skipped)
                    ++i;
            }
            else if (option == "-mm")
            {
                i += 3;
            }
            else if (option == "-imfchan")
            {
                i += 2;
                if (i <= tokens.size())
                    result.channel = ParseObjTextureChannel(tokens[i - 1]);
            }
            else if (option == "-bm")
            {
                i += 2;
                if (i <= tokens.size())
                {
                    result.bumpMultiplier = std::strtof(tokens[i - 1].c_str(), nullptr);
                    result.hasBumpMultiplier = true;
                }
            }
            else if (
                option == "-boost" ||
                option == "-blendu" ||
                option == "-blendv" ||
                option == "-cc" ||
                option == "-clamp" ||
                option == "-texres" ||
                option == "-type")
            {
                i += 2;
            }
            else
            {
                ++i;
                if (i < tokens.size() && IsFloatToken(tokens[i]))
                    ++i;
            }
        }

        return result;
    }

    std::filesystem::path ResolveObjTexturePath(const std::filesystem::path& materialFilePath, const std::string& texturePath)
    {
        std::filesystem::path path(texturePath);
        if (path.is_absolute())
            return path.lexically_normal();
        return (materialFilePath.parent_path() / path).lexically_normal();
    }

    struct ObjImageRgba8
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
    };

    bool LoadObjImageRgba8(const ObjTextureReference& texture, ObjImageRgba8& output)
    {
        if (texture.empty())
            return false;

        std::ifstream file(texture.path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            donut::log::warning("OBJ texture '%s' could not be opened for channel packing.", texture.path.string().c_str());
            return false;
        }

        const std::streamsize fileSize = file.tellg();
        if (fileSize <= 0 || fileSize > std::numeric_limits<int>::max())
        {
            donut::log::warning("OBJ texture '%s' has unsupported size for channel packing.", texture.path.string().c_str());
            return false;
        }

        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> encoded(static_cast<size_t>(fileSize));
        if (!file.read(reinterpret_cast<char*>(encoded.data()), fileSize))
        {
            donut::log::warning("OBJ texture '%s' could not be read for channel packing.", texture.path.string().c_str());
            return false;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
        if (!pixels || width <= 0 || height <= 0)
        {
            donut::log::warning("OBJ texture '%s' could not be decoded for channel packing.", texture.path.string().c_str());
            if (pixels)
                stbi_image_free(pixels);
            return false;
        }

        output.width = width;
        output.height = height;
        output.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        stbi_image_free(pixels);
        return true;
    }

    uint8_t SampleObjTextureChannel(
        const ObjImageRgba8& image,
        ObjTextureChannel channel,
        uint32_t x,
        uint32_t y,
        uint32_t targetWidth,
        uint32_t targetHeight)
    {
        if (!image.valid())
            return 255;

        const uint32_t sourceX = targetWidth > 1
            ? x * static_cast<uint32_t>(image.width - 1) / (targetWidth - 1)
            : 0;
        const uint32_t sourceY = targetHeight > 1
            ? y * static_cast<uint32_t>(image.height - 1) / (targetHeight - 1)
            : 0;
        const size_t offset = (static_cast<size_t>(sourceY) * static_cast<size_t>(image.width) + sourceX) * 4;
        const uint8_t r = image.rgba[offset + 0];
        const uint8_t g = image.rgba[offset + 1];
        const uint8_t b = image.rgba[offset + 2];
        const uint8_t a = image.rgba[offset + 3];

        switch (channel)
        {
        case ObjTextureChannel::Green: return g;
        case ObjTextureChannel::Blue: return b;
        case ObjTextureChannel::Alpha: return a;
        case ObjTextureChannel::Luminance:
            return static_cast<uint8_t>(dm::clamp(0.2126f * float(r) + 0.7152f * float(g) + 0.0722f * float(b), 0.0f, 255.0f));
        case ObjTextureChannel::Red:
        default:
            return r;
        }
    }

    std::string SanitizeObjGeneratedTextureComponent(std::string text)
    {
        if (text.empty())
            text = "default";

        for (char& c : text)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
                c = '_';
        }
        return text;
    }

    std::string MakeGeneratedObjTextureName(
        const std::filesystem::path& objFilePath,
        const std::string& materialName,
        const char* suffix)
    {
        const std::string fileName = objFilePath.stem().string()
            + "."
            + SanitizeObjGeneratedTextureComponent(materialName)
            + "."
            + suffix
            + ".png";
        return (objFilePath.parent_path() / fileName).generic_string();
    }

    void AppendObjPngBytes(void* context, void* data, int size)
    {
        if (!context || !data || size <= 0)
            return;

        auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* begin = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }

    std::shared_ptr<LoadedTexture> LoadGeneratedObjTexture(
        const std::shared_ptr<TextureCache>& textureCache,
        const std::string& generatedName,
        const std::vector<uint8_t>& rgba,
        uint32_t width,
        uint32_t height,
        bool sRGB)
    {
        if (!textureCache || rgba.empty() || width == 0 || height == 0)
            return nullptr;

        std::vector<uint8_t> encodedPng;
        const int encoded = stbi_write_png_to_func(
            AppendObjPngBytes,
            &encodedPng,
            static_cast<int>(width),
            static_cast<int>(height),
            4,
            rgba.data(),
            static_cast<int>(width * 4));

        if (!encoded || encodedPng.empty())
        {
            donut::log::warning("Failed to encode generated OBJ material texture '%s'.", generatedName.c_str());
            return nullptr;
        }

        void* pngData = malloc(encodedPng.size());
        if (!pngData)
            return nullptr;

        std::memcpy(pngData, encodedPng.data(), encodedPng.size());
        auto blob = std::make_shared<vfs::Blob>(pngData, encodedPng.size());
        return textureCache->LoadTextureFromMemoryDeferred(blob, generatedName, "image/png", sRGB);
    }

    int ResolveObjIndex(int index, size_t count)
    {
        if (index > 0)
            return index - 1;
        if (index < 0)
            return static_cast<int>(count) + index;
        return -1;
    }

    bool ParseObjIndex(const std::string& text, int& out)
    {
        if (text.empty())
        {
            out = 0;
            return true;
        }

        char* end = nullptr;
        long value = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0')
            return false;

        out = static_cast<int>(value);
        return true;
    }

    struct ObjFaceVertex
    {
        int position = -1;
        int texcoord = -1;
        int normal = -1;

        bool operator==(const ObjFaceVertex& other) const = default;
    };

    struct ObjFaceVertexHash
    {
        size_t operator()(const ObjFaceVertex& key) const noexcept
        {
            size_t h = std::hash<int>{}(key.position);
            h ^= std::hash<int>{}(key.texcoord + 0x9e3779b9 + (int)(h << 6) + (int)(h >> 2));
            h ^= std::hash<int>{}(key.normal + 0x7f4a7c15 + (int)(h << 6) + (int)(h >> 2));
            return h;
        }
    };

    bool ParseObjFaceVertex(const std::string& token, size_t positionCount, size_t texcoordCount, size_t normalCount, ObjFaceVertex& out)
    {
        std::array<std::string, 3> parts;
        size_t partIndex = 0;
        size_t partStart = 0;
        for (size_t i = 0; i <= token.size(); ++i)
        {
            if (i == token.size() || token[i] == '/')
            {
                if (partIndex >= parts.size())
                    return false;
                parts[partIndex++] = token.substr(partStart, i - partStart);
                partStart = i + 1;
            }
        }

        int position = 0;
        if (!ParseObjIndex(parts[0], position))
            return false;

        out.position = ResolveObjIndex(position, positionCount);
        if (out.position < 0 || out.position >= static_cast<int>(positionCount))
            return false;

        if (partIndex > 1 && !parts[1].empty())
        {
            int texcoord = 0;
            if (!ParseObjIndex(parts[1], texcoord))
                return false;
            out.texcoord = ResolveObjIndex(texcoord, texcoordCount);
            if (out.texcoord < 0 || out.texcoord >= static_cast<int>(texcoordCount))
                return false;
        }

        if (partIndex > 2 && !parts[2].empty())
        {
            int normal = 0;
            if (!ParseObjIndex(parts[2], normal))
                return false;
            out.normal = ResolveObjIndex(normal, normalCount);
            if (out.normal < 0 || out.normal >= static_cast<int>(normalCount))
                return false;
        }

        return true;
    }

    dm::float3 NormalizeOrFallback(dm::float3 value, dm::float3 fallback)
    {
        const float len = dm::length(value);
        return len > 1e-20f ? value / len : fallback;
    }

    dm::float3 BuildFallbackTangent(dm::float3 normal)
    {
        const dm::float3 axis = std::abs(normal.z) < 0.999f
            ? dm::float3(0.0f, 0.0f, 1.0f)
            : dm::float3(0.0f, 1.0f, 0.0f);
        return NormalizeOrFallback(dm::cross(axis, normal), dm::float3(1.0f, 0.0f, 0.0f));
    }

    struct ObjMaterialInfo
    {
        std::string name = "default";
        dm::float3 baseColor = dm::float3(0.8f);
        dm::float3 specularColor = dm::float3(0.0f);
        dm::float3 emissiveColor = dm::float3(0.0f);
        float roughness = 0.5f;
        float metalness = 0.0f;
        float opacity = 1.0f;
        float transmissionFactor = 0.0f;
        float normalTextureScale = 1.0f;
        float ior = 1.5f;
        bool hasBaseColor = false;
        bool hasSpecularColor = false;
        bool hasEmissiveColor = false;
        bool hasRoughness = false;
        bool hasMetalness = false;
        bool hasTransmission = false;
        bool useSpecularGlossModel = false;
        ObjTextureReference baseTexture;
        ObjTextureReference packedMetalRoughTexture;
        ObjTextureReference roughnessTexture;
        ObjTextureReference metalnessTexture;
        ObjTextureReference specularTexture;
        ObjTextureReference glossinessTexture;
        ObjTextureReference normalTexture;
        ObjTextureReference emissiveTexture;
        ObjTextureReference occlusionTexture;
        ObjTextureReference opacityTexture;
        ObjTextureReference transmissionTexture;
    };

    std::unordered_map<std::string, ObjMaterialInfo> LoadObjMaterialLibrary(const std::filesystem::path& filePath)
    {
        std::unordered_map<std::string, ObjMaterialInfo> materials;
        std::ifstream file(filePath);
        if (!file)
        {
            donut::log::warning("OBJ material library '%s' could not be opened.", filePath.string().c_str());
            return materials;
        }

        ObjMaterialInfo* current = nullptr;
        std::string line;
        while (std::getline(file, line))
        {
            line = TrimString(line);
            if (line.empty() || line[0] == '#')
                continue;

            const auto tokens = SplitWhitespace(line);
            if (tokens.empty())
                continue;

            const std::string keyword = ToLowerString(tokens[0]);

            if (keyword == "newmtl" && tokens.size() >= 2)
            {
                ObjMaterialInfo material;
                material.name = StripMatchingQuotes(JoinTokens(tokens, 1));
                auto [it, inserted] = materials.insert_or_assign(material.name, material);
                current = &it->second;
            }
            else if (current && (keyword == "kd" || keyword == "ke" || keyword == "ks") && tokens.size() >= 4)
            {
                dm::float3 color(
                    std::strtof(tokens[1].c_str(), nullptr),
                    std::strtof(tokens[2].c_str(), nullptr),
                    std::strtof(tokens[3].c_str(), nullptr));
                if (keyword == "kd")
                {
                    current->baseColor = color;
                    current->hasBaseColor = true;
                }
                else if (keyword == "ks")
                {
                    current->specularColor = color;
                    current->hasSpecularColor = true;
                }
                else
                {
                    current->emissiveColor = color;
                    current->hasEmissiveColor = true;
                }
            }
            else if (current && keyword == "ns" && tokens.size() >= 2)
            {
                const float ns = std::max(0.0f, std::strtof(tokens[1].c_str(), nullptr));
                current->roughness = dm::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.02f, 1.0f);
                current->hasRoughness = true;
            }
            else if (current && keyword == "pr" && tokens.size() >= 2)
            {
                current->roughness = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.02f, 1.0f);
                current->hasRoughness = true;
                current->useSpecularGlossModel = false;
            }
            else if (current && keyword == "pm" && tokens.size() >= 2)
            {
                current->metalness = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.0f, 1.0f);
                current->hasMetalness = true;
                current->useSpecularGlossModel = false;
            }
            else if (current && keyword == "ni" && tokens.size() >= 2)
            {
                current->ior = std::max(1.0f, std::strtof(tokens[1].c_str(), nullptr));
            }
            else if (current && (keyword == "d" || keyword == "tr") && tokens.size() >= 2)
            {
                const float value = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.0f, 1.0f);
                current->opacity = keyword == "tr" ? 1.0f - value : value;
            }
            else if (current && keyword == "tf" && tokens.size() >= 4)
            {
                const dm::float3 color(
                    std::strtof(tokens[1].c_str(), nullptr),
                    std::strtof(tokens[2].c_str(), nullptr),
                    std::strtof(tokens[3].c_str(), nullptr));
                current->transmissionFactor = dm::clamp(dm::maxComponent(color), 0.0f, 1.0f);
                current->hasTransmission = true;
            }
            else if (current && (
                keyword == "map_kd" ||
                keyword == "map_basecolor" ||
                keyword == "map_ka" ||
                keyword == "map_ao" ||
                keyword == "map_occlusion" ||
                keyword == "map_ks" ||
                keyword == "map_ns" ||
                keyword == "map_pr" ||
                keyword == "map_roughness" ||
                keyword == "map_pm" ||
                keyword == "map_metallic" ||
                keyword == "map_metalness" ||
                keyword == "map_orm" ||
                keyword == "map_mr" ||
                keyword == "map_metallicroughness" ||
                keyword == "map_occlusionroughnessmetallic" ||
                keyword == "map_bump" ||
                keyword == "bump" ||
                keyword == "norm" ||
                keyword == "map_normal" ||
                keyword == "map_ke" ||
                keyword == "map_emissive" ||
                keyword == "map_d" ||
                keyword == "map_opacity" ||
                keyword == "map_tf"))
            {
                ObjTextureReference texture = ExtractObjTextureReference(tokens);
                if (texture.path.empty())
                    continue;

                texture.path = ResolveObjTexturePath(filePath, texture.path.string());

                if (keyword == "map_kd" || keyword == "map_basecolor")
                    current->baseTexture = texture;
                else if (keyword == "map_ka" || keyword == "map_ao" || keyword == "map_occlusion")
                    current->occlusionTexture = texture;
                else if (keyword == "map_ke" || keyword == "map_emissive")
                {
                    current->emissiveTexture = texture;
                    if (!current->hasEmissiveColor)
                    {
                        current->emissiveColor = dm::float3(1.0f);
                        current->hasEmissiveColor = true;
                    }
                }
                else if (keyword == "map_d" || keyword == "map_opacity")
                    current->opacityTexture = texture;
                else if (keyword == "map_tf")
                    current->transmissionTexture = texture;
                else if (keyword == "map_bump" || keyword == "bump" || keyword == "norm" || keyword == "map_normal")
                {
                    current->normalTexture = texture;
                    if (texture.hasBumpMultiplier)
                        current->normalTextureScale = texture.bumpMultiplier;
                }
                else if (keyword == "map_pm" || keyword == "map_metallic" || keyword == "map_metalness")
                {
                    current->metalnessTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (keyword == "map_pr" || keyword == "map_roughness")
                {
                    current->roughnessTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (
                    keyword == "map_orm" ||
                    keyword == "map_mr" ||
                    keyword == "map_metallicroughness" ||
                    keyword == "map_occlusionroughnessmetallic")
                {
                    current->packedMetalRoughTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (keyword == "map_ns")
                {
                    current->glossinessTexture = texture;
                    if (current->roughnessTexture.empty() && current->metalnessTexture.empty() && current->packedMetalRoughTexture.empty())
                        current->useSpecularGlossModel = true;
                }
                else if (keyword == "map_ks")
                {
                    current->specularTexture = texture;
                    if (current->roughnessTexture.empty() && current->metalnessTexture.empty() && current->packedMetalRoughTexture.empty())
                        current->useSpecularGlossModel = true;
                }
            }
        }

        return materials;
    }

    bool SelectObjPackedTextureSize(
        const ObjImageRgba8& a,
        const ObjImageRgba8& b,
        const ObjImageRgba8& c,
        uint32_t& width,
        uint32_t& height)
    {
        const ObjImageRgba8* images[] = { &a, &b, &c };
        for (const ObjImageRgba8* image : images)
        {
            if (image->valid())
            {
                width = static_cast<uint32_t>(image->width);
                height = static_cast<uint32_t>(image->height);
                return true;
            }
        }

        return false;
    }

    std::shared_ptr<LoadedTexture> BuildObjMetalRoughTexture(
        const ObjMaterialInfo& material,
        const std::filesystem::path& objFilePath,
        const std::shared_ptr<TextureCache>& textureCache)
    {
        if (material.occlusionTexture.empty() && material.roughnessTexture.empty() && material.metalnessTexture.empty())
            return nullptr;

        ObjImageRgba8 occlusionImage;
        ObjImageRgba8 roughnessImage;
        ObjImageRgba8 metalnessImage;

        LoadObjImageRgba8(material.occlusionTexture, occlusionImage);
        LoadObjImageRgba8(material.roughnessTexture, roughnessImage);
        LoadObjImageRgba8(material.metalnessTexture, metalnessImage);

        uint32_t width = 0;
        uint32_t height = 0;
        if (!SelectObjPackedTextureSize(occlusionImage, roughnessImage, metalnessImage, width, height))
            return nullptr;

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                rgba[offset + 0] = occlusionImage.valid()
                    ? SampleObjTextureChannel(occlusionImage, material.occlusionTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 1] = roughnessImage.valid()
                    ? SampleObjTextureChannel(roughnessImage, material.roughnessTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 2] = metalnessImage.valid()
                    ? SampleObjTextureChannel(metalnessImage, material.metalnessTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 3] = 255;
            }
        }

        return LoadGeneratedObjTexture(
            textureCache,
            MakeGeneratedObjTextureName(objFilePath, material.name, "orm"),
            rgba,
            width,
            height,
            false);
    }

    std::shared_ptr<LoadedTexture> BuildObjSpecGlossTexture(
        const ObjMaterialInfo& material,
        const std::filesystem::path& objFilePath,
        const std::shared_ptr<TextureCache>& textureCache)
    {
        if (material.specularTexture.empty() && material.glossinessTexture.empty())
            return nullptr;

        ObjImageRgba8 specularImage;
        ObjImageRgba8 glossinessImage;

        LoadObjImageRgba8(material.specularTexture, specularImage);
        LoadObjImageRgba8(material.glossinessTexture, glossinessImage);

        uint32_t width = 0;
        uint32_t height = 0;
        if (!SelectObjPackedTextureSize(specularImage, glossinessImage, ObjImageRgba8{}, width, height))
            return nullptr;

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                if (specularImage.valid())
                {
                    const uint32_t sourceX = width > 1
                        ? x * static_cast<uint32_t>(specularImage.width - 1) / (width - 1)
                        : 0;
                    const uint32_t sourceY = height > 1
                        ? y * static_cast<uint32_t>(specularImage.height - 1) / (height - 1)
                        : 0;
                    const size_t sourceOffset = (static_cast<size_t>(sourceY) * static_cast<size_t>(specularImage.width) + sourceX) * 4;
                    rgba[offset + 0] = specularImage.rgba[sourceOffset + 0];
                    rgba[offset + 1] = specularImage.rgba[sourceOffset + 1];
                    rgba[offset + 2] = specularImage.rgba[sourceOffset + 2];
                }
                else
                {
                    rgba[offset + 0] = 255;
                    rgba[offset + 1] = 255;
                    rgba[offset + 2] = 255;
                }

                rgba[offset + 3] = glossinessImage.valid()
                    ? SampleObjTextureChannel(glossinessImage, material.glossinessTexture.channel, x, y, width, height)
                    : 255;
            }
        }

        return LoadGeneratedObjTexture(
            textureCache,
            MakeGeneratedObjTextureName(objFilePath, material.name, "specgloss"),
            rgba,
            width,
            height,
            true);
    }
}

void Sample::HandleDroppedFiles()
{
    if (m_ui.PendingDroppedFiles.empty())
        return;

    auto files = std::move(m_ui.PendingDroppedFiles);
    m_ui.PendingDroppedFiles.clear();

    for (const auto& filePath : files)
    {
        std::filesystem::path path(filePath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        if (ext == ".ply")
        {
            log::info("Drag-drop: loading Gaussian Splat file '%s'", filePath.c_str());
            if (LoadGaussianSplatFile(path))
                log::info("Gaussian Splat loaded successfully: %d splats across %d objects", GetGaussianSplatCount(), GetGaussianSplatObjectCount());
            else
                log::error("Failed to load Gaussian Splat file '%s'", filePath.c_str());
        }
        else if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
        {
            log::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (LoadMeshFile(path))
                log::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                log::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            log::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj)", ext.c_str());
        }
    }
}

bool Sample::LoadMeshFile(const std::filesystem::path& filePath)
{
    if (!m_scene || !m_shaderFactory || !m_TextureCache)
    {
        log::error("Cannot load mesh: scene, shader factory, or texture cache not initialized.");
        return false;
    }

    std::filesystem::path absPath = filePath;
    if (!absPath.is_absolute())
        absPath = std::filesystem::absolute(absPath);

    if (!std::filesystem::exists(absPath))
    {
        log::error("File does not exist: '%s'", absPath.string().c_str());
        return false;
    }

    std::string ext = absPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (ext == ".gltf" || ext == ".glb")
        return LoadGltfMeshFile(absPath);
    if (ext == ".obj")
        return LoadObjMeshFile(absPath);

    log::error("Unsupported mesh file type '%s'.", ext.c_str());
    return false;
}

bool Sample::LoadGltfMeshFile(const std::filesystem::path& filePath)
{
    auto fs = std::make_shared<vfs::NativeFileSystem>();
    auto sceneTypeFactory = std::make_shared<ExtendedSceneTypeFactory>();
    auto importer = std::make_shared<engine::GltfImporter>(fs, sceneTypeFactory);

    engine::SceneLoadingStats stats;
    engine::SceneImportResult importResult;

    std::filesystem::path sceneDirectory;
    if (m_currentScenePath != std::filesystem::path(c_InlineSceneSentinel))
        sceneDirectory = m_currentScenePath.parent_path();

    if (!importer->Load(filePath, *m_TextureCache, stats, nullptr, importResult, sceneDirectory))
    {
        log::error("GltfImporter failed to load '%s'", filePath.string().c_str());
        return false;
    }

    if (!importResult.rootNode)
    {
        log::error("GltfImporter produced no root node for '%s'", filePath.string().c_str());
        return false;
    }

    importResult.rootNode->SetName(filePath.stem().string());

    auto importedRoot = m_scene->GetSceneGraph()->Attach(m_scene->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}

static bool LoadObjModelFile(
    const std::filesystem::path& filePath,
    const std::shared_ptr<TextureCache>& textureCache,
    const std::shared_ptr<SceneTypeFactory>& sceneTypeFactory,
    SceneImportResult& result)
{
    std::ifstream file(filePath);
    if (!file)
    {
        log::error("OBJ file could not be opened: '%s'", filePath.string().c_str());
        return false;
    }

    std::vector<dm::float3> positions;
    std::vector<dm::float2> texcoords;
    std::vector<dm::float3> normals;
    std::unordered_map<std::string, ObjMaterialInfo> materials;
    materials["default"] = ObjMaterialInfo{};

    struct ObjGroup
    {
        std::string materialName = "default";
        std::vector<uint32_t> indices;
        dm::box3 bounds = dm::box3::empty();
    };

    std::vector<ObjGroup> groups;
    std::unordered_map<std::string, size_t> groupByMaterial;
    auto getGroup = [&](const std::string& materialName) -> ObjGroup&
    {
        auto found = groupByMaterial.find(materialName);
        if (found != groupByMaterial.end())
            return groups[found->second];

        ObjGroup group;
        group.materialName = materialName;
        groupByMaterial[materialName] = groups.size();
        groups.push_back(group);
        return groups.back();
    };

    ObjGroup* currentGroup = &getGroup("default");

    auto mesh = std::static_pointer_cast<MeshInfoEx>(sceneTypeFactory->CreateMesh());
    mesh->name = filePath.stem().string();
    mesh->type = MeshType::Triangles;
    mesh->buffers = std::make_shared<BufferGroup>();
    mesh->objectSpaceBounds = dm::box3::empty();

    std::unordered_map<ObjFaceVertex, uint32_t, ObjFaceVertexHash> vertexMap;
    std::vector<int> vertexNormalRefs;

    auto addVertex = [&](const ObjFaceVertex& key) -> uint32_t
    {
        auto found = vertexMap.find(key);
        if (found != vertexMap.end())
            return found->second;

        const uint32_t index = static_cast<uint32_t>(mesh->buffers->positionData.size());
        vertexMap[key] = index;

        const dm::float3 position = positions[key.position];
        mesh->buffers->positionData.push_back(position);
        mesh->buffers->texcoord1Data.push_back(key.texcoord >= 0 ? texcoords[key.texcoord] : dm::float2(0.0f));
        mesh->DeformationSourcePositionIndices.push_back(static_cast<uint32_t>(key.position));
        vertexNormalRefs.push_back(key.normal);
        mesh->objectSpaceBounds |= position;

        return index;
    };

    std::string currentMaterial = "default";
    std::string line;
    uint32_t lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        line = TrimString(line);
        if (line.empty() || line[0] == '#')
            continue;

        const auto tokens = SplitWhitespace(line);
        if (tokens.empty())
            continue;

        const std::string keyword = ToLowerString(tokens[0]);

        if (keyword == "v" && tokens.size() >= 4)
        {
            positions.push_back(dm::float3(
                std::strtof(tokens[1].c_str(), nullptr),
                std::strtof(tokens[2].c_str(), nullptr),
                std::strtof(tokens[3].c_str(), nullptr)));
        }
        else if (keyword == "vt" && tokens.size() >= 3)
        {
            texcoords.push_back(dm::float2(
                std::strtof(tokens[1].c_str(), nullptr),
                1.0f - std::strtof(tokens[2].c_str(), nullptr)));
        }
        else if (keyword == "vn" && tokens.size() >= 4)
        {
            normals.push_back(NormalizeOrFallback(dm::float3(
                std::strtof(tokens[1].c_str(), nullptr),
                std::strtof(tokens[2].c_str(), nullptr),
                std::strtof(tokens[3].c_str(), nullptr)), dm::float3(0.0f, 1.0f, 0.0f)));
        }
        else if (keyword == "mtllib" && tokens.size() >= 2)
        {
            const std::filesystem::path materialLibrary = StripMatchingQuotes(JoinTokens(tokens, 1));
            auto loaded = LoadObjMaterialLibrary(filePath.parent_path() / materialLibrary);
            for (auto& [name, material] : loaded)
                materials.insert_or_assign(name, material);
        }
        else if (keyword == "usemtl" && tokens.size() >= 2)
        {
            currentMaterial = StripMatchingQuotes(JoinTokens(tokens, 1));
            if (!materials.contains(currentMaterial))
            {
                ObjMaterialInfo material;
                material.name = currentMaterial;
                materials[currentMaterial] = material;
            }
            currentGroup = &getGroup(currentMaterial);
        }
        else if (keyword == "f" && tokens.size() >= 4)
        {
            std::vector<uint32_t> polygon;
            polygon.reserve(tokens.size() - 1);

            bool validFace = true;
            for (size_t i = 1; i < tokens.size(); ++i)
            {
                ObjFaceVertex key;
                if (!ParseObjFaceVertex(tokens[i], positions.size(), texcoords.size(), normals.size(), key))
                {
                    log::warning("Skipping malformed OBJ face at '%s':%u", filePath.string().c_str(), lineNumber);
                    validFace = false;
                    break;
                }
                polygon.push_back(addVertex(key));
            }

            if (!validFace || polygon.size() < 3)
                continue;

            for (size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                const uint32_t tri[3] = { polygon[0], polygon[i], polygon[i + 1] };
                currentGroup->indices.insert(currentGroup->indices.end(), std::begin(tri), std::end(tri));
                currentGroup->bounds |= mesh->buffers->positionData[tri[0]];
                currentGroup->bounds |= mesh->buffers->positionData[tri[1]];
                currentGroup->bounds |= mesh->buffers->positionData[tri[2]];
            }
        }
    }

    if (mesh->buffers->positionData.empty())
    {
        log::error("OBJ file '%s' has no usable vertices.", filePath.string().c_str());
        return false;
    }

    std::vector<dm::float3> accumulatedNormals(mesh->buffers->positionData.size(), dm::float3(0.0f));
    for (const ObjGroup& group : groups)
    {
        for (size_t i = 0; i + 2 < group.indices.size(); i += 3)
        {
            const uint32_t i0 = group.indices[i + 0];
            const uint32_t i1 = group.indices[i + 1];
            const uint32_t i2 = group.indices[i + 2];
            const dm::float3& p0 = mesh->buffers->positionData[i0];
            const dm::float3& p1 = mesh->buffers->positionData[i1];
            const dm::float3& p2 = mesh->buffers->positionData[i2];
            const dm::float3 faceNormal = NormalizeOrFallback(dm::cross(p1 - p0, p2 - p0), dm::float3(0.0f, 1.0f, 0.0f));
            accumulatedNormals[i0] += faceNormal;
            accumulatedNormals[i1] += faceNormal;
            accumulatedNormals[i2] += faceNormal;
        }
    }

    std::vector<dm::float3> resolvedNormals;
    resolvedNormals.reserve(mesh->buffers->positionData.size());
    mesh->buffers->normalData.reserve(mesh->buffers->positionData.size());
    for (size_t i = 0; i < mesh->buffers->positionData.size(); ++i)
    {
        const int normalIndex = vertexNormalRefs[i];
        const dm::float3 normal = normalIndex >= 0
            ? normals[normalIndex]
            : NormalizeOrFallback(accumulatedNormals[i], dm::float3(0.0f, 1.0f, 0.0f));
        resolvedNormals.push_back(normal);
        mesh->buffers->normalData.push_back(dm::vectorToSnorm8(normal));
    }

    std::vector<dm::float3> accumulatedTangents(mesh->buffers->positionData.size(), dm::float3(0.0f));
    std::vector<dm::float3> accumulatedBitangents(mesh->buffers->positionData.size(), dm::float3(0.0f));
    for (const ObjGroup& group : groups)
    {
        for (size_t i = 0; i + 2 < group.indices.size(); i += 3)
        {
            const uint32_t i0 = group.indices[i + 0];
            const uint32_t i1 = group.indices[i + 1];
            const uint32_t i2 = group.indices[i + 2];
            const dm::float3& p0 = mesh->buffers->positionData[i0];
            const dm::float3& p1 = mesh->buffers->positionData[i1];
            const dm::float3& p2 = mesh->buffers->positionData[i2];
            const dm::float2& uv0 = mesh->buffers->texcoord1Data[i0];
            const dm::float2& uv1 = mesh->buffers->texcoord1Data[i1];
            const dm::float2& uv2 = mesh->buffers->texcoord1Data[i2];

            const dm::float3 edge1 = p1 - p0;
            const dm::float3 edge2 = p2 - p0;
            const dm::float2 deltaUv1 = uv1 - uv0;
            const dm::float2 deltaUv2 = uv2 - uv0;
            const float determinant = deltaUv1.x * deltaUv2.y - deltaUv2.x * deltaUv1.y;
            if (std::abs(determinant) <= 1e-20f)
                continue;

            const float invDeterminant = 1.0f / determinant;
            const dm::float3 tangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * invDeterminant;
            const dm::float3 bitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * invDeterminant;

            accumulatedTangents[i0] += tangent;
            accumulatedTangents[i1] += tangent;
            accumulatedTangents[i2] += tangent;
            accumulatedBitangents[i0] += bitangent;
            accumulatedBitangents[i1] += bitangent;
            accumulatedBitangents[i2] += bitangent;
        }
    }

    mesh->buffers->tangentData.reserve(mesh->buffers->positionData.size());
    for (size_t i = 0; i < mesh->buffers->positionData.size(); ++i)
    {
        const dm::float3 normal = resolvedNormals[i];
        dm::float3 tangent = accumulatedTangents[i] - normal * dm::dot(normal, accumulatedTangents[i]);
        tangent = NormalizeOrFallback(tangent, BuildFallbackTangent(normal));
        const float handedness = dm::dot(dm::cross(normal, tangent), accumulatedBitangents[i]) < 0.0f ? -1.0f : 1.0f;
        mesh->buffers->tangentData.push_back(dm::vectorToSnorm8(dm::float4(tangent, handedness)));
    }

    auto loadObjTexture = [&](const std::filesystem::path& texturePath, bool sRGB) -> std::shared_ptr<LoadedTexture>
    {
        if (texturePath.empty())
            return nullptr;

        if (!std::filesystem::exists(texturePath))
        {
            log::warning("OBJ texture '%s' referenced by '%s' was not found.",
                texturePath.string().c_str(), filePath.string().c_str());
            return nullptr;
        }

        return textureCache->LoadTextureFromFileDeferred(texturePath, sRGB);
    };

    auto loadObjTextureReference = [&](const ObjTextureReference& texture, bool sRGB) -> std::shared_ptr<LoadedTexture>
    {
        return loadObjTexture(texture.path, sRGB);
    };

    for (const ObjGroup& group : groups)
    {
        if (group.indices.empty())
            continue;

        const ObjMaterialInfo& objMaterial = materials.contains(group.materialName)
            ? materials[group.materialName]
            : materials["default"];

        auto material = std::dynamic_pointer_cast<MaterialEx>(sceneTypeFactory->CreateMaterial());
        material->name = objMaterial.name;
        material->modelFileName = filePath.string();
        const bool hasPbrRoughnessTexture = !objMaterial.roughnessTexture.empty() || !objMaterial.packedMetalRoughTexture.empty();
        const bool hasPbrMetalnessTexture = !objMaterial.metalnessTexture.empty() || !objMaterial.packedMetalRoughTexture.empty();
        const bool hasSpecGlossTexture = !objMaterial.specularTexture.empty() || !objMaterial.glossinessTexture.empty();

        material->baseOrDiffuseColor = !objMaterial.baseTexture.empty() && !objMaterial.hasBaseColor
            ? dm::float3(1.0f)
            : objMaterial.baseColor;
        material->specularColor = hasSpecGlossTexture && objMaterial.useSpecularGlossModel && !objMaterial.hasSpecularColor
            ? dm::float3(1.0f)
            : objMaterial.specularColor;
        material->emissiveColor = objMaterial.emissiveColor;
        if (objMaterial.useSpecularGlossModel)
        {
            material->roughness = !objMaterial.glossinessTexture.empty() && !objMaterial.hasRoughness
                ? 0.0f
                : objMaterial.roughness;
            material->metalness = objMaterial.metalness;
        }
        else
        {
            material->roughness = hasPbrRoughnessTexture && !objMaterial.hasRoughness
                ? 1.0f
                : objMaterial.roughness;
            material->metalness = hasPbrMetalnessTexture && !objMaterial.hasMetalness
                ? 1.0f
                : objMaterial.metalness;
        }
        material->opacity = objMaterial.opacity;
        material->normalTextureScale = objMaterial.normalTextureScale;
        material->useSpecularGlossModel = objMaterial.useSpecularGlossModel;
        material->metalnessInRedChannel = false;
        material->transmissionFactor = !objMaterial.transmissionTexture.empty() && !objMaterial.hasTransmission
            ? 1.0f
            : objMaterial.transmissionFactor;
        material->baseOrDiffuseTexture = loadObjTextureReference(objMaterial.baseTexture, true);
        if (objMaterial.useSpecularGlossModel)
        {
            material->metalRoughOrSpecularTexture = BuildObjSpecGlossTexture(objMaterial, filePath, textureCache);
            if (!material->metalRoughOrSpecularTexture)
                material->metalRoughOrSpecularTexture = loadObjTextureReference(objMaterial.specularTexture, true);
        }
        else
        {
            material->metalRoughOrSpecularTexture = BuildObjMetalRoughTexture(objMaterial, filePath, textureCache);
            if (!material->metalRoughOrSpecularTexture)
                material->metalRoughOrSpecularTexture = loadObjTextureReference(objMaterial.packedMetalRoughTexture, false);
        }
        material->normalTexture = loadObjTextureReference(objMaterial.normalTexture, false);
        material->emissiveTexture = loadObjTextureReference(objMaterial.emissiveTexture, true);
        material->occlusionTexture = loadObjTextureReference(objMaterial.occlusionTexture, false);
        material->opacityTexture = loadObjTextureReference(objMaterial.opacityTexture, false);
        material->transmissionTexture = loadObjTextureReference(objMaterial.transmissionTexture, false);
        if (material->transmissionFactor > 0.0f || material->transmissionTexture)
            material->domain = objMaterial.opacity < 0.999f ? MaterialDomain::TransmissiveAlphaBlended : MaterialDomain::Transmissive;
        else
            material->domain = objMaterial.opacity < 0.999f ? MaterialDomain::AlphaBlended : MaterialDomain::Opaque;
        LocalConfig::PostMaterialLoad(*material);

        auto geometry = std::static_pointer_cast<MeshGeometryEx>(sceneTypeFactory->CreateMeshGeometry());
        geometry->material = material;
        geometry->indexOffsetInMesh = static_cast<uint32_t>(mesh->buffers->indexData.size());
        geometry->vertexOffsetInMesh = 0;
        geometry->numIndices = static_cast<uint32_t>(group.indices.size());
        geometry->numVertices = static_cast<uint32_t>(mesh->buffers->positionData.size());
        geometry->objectSpaceBounds = group.bounds;

        mesh->buffers->indexData.insert(mesh->buffers->indexData.end(), group.indices.begin(), group.indices.end());
        mesh->geometries.push_back(geometry);
    }

    if (mesh->geometries.empty())
    {
        log::error("OBJ file '%s' has no usable triangle faces.", filePath.string().c_str());
        return false;
    }

    mesh->totalVertices = static_cast<uint32_t>(mesh->buffers->positionData.size());
    mesh->totalIndices = static_cast<uint32_t>(mesh->buffers->indexData.size());

    auto importedRoot = std::make_shared<SceneGraphNode>();
    importedRoot->SetName(filePath.stem().string());
    importedRoot->SetLeaf(sceneTypeFactory->CreateMeshInstance(mesh));

    result.rootNode = importedRoot;
    return true;
}

bool Sample::LoadObjMeshFile(const std::filesystem::path& filePath)
{
    SceneImportResult importResult;
    auto sceneTypeFactory = std::make_shared<ExtendedSceneTypeFactory>();
    if (!LoadObjModelFile(filePath, m_TextureCache, sceneTypeFactory, importResult))
        return false;

    auto importedRoot = m_scene->GetSceneGraph()->Attach(m_scene->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}

bool ExtendedScene::LoadModelFile(
    const std::filesystem::path& fileName,
    ThreadPool* threadPool,
    SceneImportResult& result)
{
    std::string ext = fileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (ext == ".obj")
        return LoadObjModelFile(fileName, m_TextureCache, m_SceneTypeFactory, result);

    return Scene::LoadModelFile(fileName, threadPool, result);
}

void Sample::FinalizeRuntimeSceneMutation(const std::shared_ptr<donut::engine::SceneGraphNode>& importedRoot)
{
    if (importedRoot)
    {
        std::unordered_set<donut::engine::Material*> processedMaterials;
        SceneGraphWalker walker(importedRoot.get());
        while (walker)
        {
            auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(walker->GetLeaf());
            if (meshInstance && meshInstance->GetMesh())
            {
                for (const auto& geometry : meshInstance->GetMesh()->geometries)
                {
                    if (geometry->material && processedMaterials.insert(geometry->material.get()).second)
                        LocalConfig::PostMaterialLoad(*geometry->material);
                }
            }
            walker.Next(true);
        }
    }

    m_scene->FinishedLoading(GetFrameIndex());

    if (m_materialsBaker != nullptr)
        m_materialsBaker->SceneReloaded();
    if (m_lightsBaker != nullptr)
        m_lightsBaker->SceneReloaded();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneLoaded(*m_scene);

    RequestFullRebuild();
}

bool Sample::DeleteSceneNode(const std::shared_ptr<SceneGraphNode>& node)
{
    if (node == nullptr || m_scene == nullptr)
        return false;

    auto sceneGraph = m_scene->GetSceneGraph();
    if (sceneGraph == nullptr)
        return false;

    auto rootNode = sceneGraph->GetRootNode();
    if (rootNode == nullptr || node == rootNode)
        return false;

    if (node->GetGraph() != sceneGraph || node->GetParent() == nullptr)
        return false;

    GetDevice()->waitForIdle();

    bool removedGaussianSplat = false;
    auto removedSplatBegin = std::remove_if(
        m_gaussianSplatSceneObjects.begin(),
        m_gaussianSplatSceneObjects.end(),
        [&node, &removedGaussianSplat](const GaussianSplatSceneObject& object)
        {
            auto objectNode = object.node.lock();
            const bool remove = objectNode != nullptr && NodeSubtreeContains(node.get(), objectNode.get());
            removedGaussianSplat = removedGaussianSplat || remove;
            return remove;
        });
    if (removedSplatBegin != m_gaussianSplatSceneObjects.end())
        m_gaussianSplatSceneObjects.erase(removedSplatBegin, m_gaussianSplatSceneObjects.end());

    sceneGraph->Detach(node, true);
    m_scene->FinishedLoading(GetFrameIndex());

    m_lights.clear();
    for (auto light : sceneGraph->GetLights())
        m_lights.push_back(light);

    if (m_ui.TogglableNodes != nullptr)
    {
        m_ui.TogglableNodes->clear();
        UpdateTogglableNodes(*m_ui.TogglableNodes, rootNode.get());
    }

    m_ui.SelectedMaterial = nullptr;
    m_ui.SelectedNode = nullptr;
    m_ui.InspectorRotationNode.reset();
    m_ui.InspectorRotationEulerValid = false;
    m_ui.SelectedGaussianSplat = false;

    if (removedGaussianSplat)
    {
        UpdateGaussianSplatUIState();
        m_gaussianSplatEmissionProxies.clear();
        m_gaussianSplatTemporalReset = true;
    }

    if (m_materialsBaker != nullptr)
        m_materialsBaker->SceneReloaded();
    if (m_lightsBaker != nullptr)
        m_lightsBaker->SceneReloaded();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneLoaded(*m_scene);

    RequestFullRebuild();
    return true;
}


void Sample::RequestFullRebuild()
{
    m_ui.AccelerationStructRebuildRequested = true;
    m_ui.ShaderReloadRequested = true;
    m_ui.ShaderAndACRefreshDelayedRequest = 0.0f;
    m_ui.ResetAccumulation = true;
    m_bindingSet = nullptr;
    if (m_bindingCache)
        m_bindingCache->Clear();
}

struct UniquePositionMapForDeform
{
    std::vector<float3> uniquePositions;
    std::vector<uint32_t> renderToUnique;
};

static std::array<uint32_t, 3> PositionKeyForDeform(const float3& p)
{
    std::array<uint32_t, 3> key{};
    std::memcpy(&key[0], &p.x, sizeof(uint32_t));
    std::memcpy(&key[1], &p.y, sizeof(uint32_t));
    std::memcpy(&key[2], &p.z, sizeof(uint32_t));
    return key;
}

static UniquePositionMapForDeform BuildUniquePositionMapForDeform(
    const std::vector<float3>& renderVertices,
    const std::vector<uint32_t>* sourcePositionIndices = nullptr)
{
    if (sourcePositionIndices && sourcePositionIndices->size() == renderVertices.size())
    {
        UniquePositionMapForDeform result;
        result.renderToUnique.resize(renderVertices.size());

        std::vector<size_t> renderOrder;
        renderOrder.reserve(renderVertices.size());
        for (size_t i = 0; i < renderVertices.size(); ++i)
            renderOrder.push_back(i);

        std::sort(renderOrder.begin(), renderOrder.end(),
            [&](size_t a, size_t b)
            {
                const uint32_t sourceA = (*sourcePositionIndices)[a];
                const uint32_t sourceB = (*sourcePositionIndices)[b];
                return sourceA == sourceB ? a < b : sourceA < sourceB;
            });

        std::unordered_map<uint32_t, uint32_t> uniqueLookup;
        uniqueLookup.reserve(renderVertices.size());

        for (size_t renderIndex : renderOrder)
        {
            const uint32_t sourceIndex = (*sourcePositionIndices)[renderIndex];
            auto found = uniqueLookup.find(sourceIndex);
            if (found == uniqueLookup.end())
            {
                const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
                uniqueLookup.emplace(sourceIndex, uniqueIndex);
                result.uniquePositions.push_back(renderVertices[renderIndex]);
                result.renderToUnique[renderIndex] = uniqueIndex;
            }
            else
            {
                result.renderToUnique[renderIndex] = found->second;
            }
        }

        return result;
    }

    struct KeyHash
    {
        size_t operator()(const std::array<uint32_t, 3>& key) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(key[0]);
            h ^= std::hash<uint32_t>{}(key[1]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(key[2]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    UniquePositionMapForDeform result;
    result.renderToUnique.reserve(renderVertices.size());

    std::unordered_map<std::array<uint32_t, 3>, uint32_t, KeyHash> uniqueLookup;
    uniqueLookup.reserve(renderVertices.size());

    for (const float3& vertex : renderVertices)
    {
        const auto key = PositionKeyForDeform(vertex);
        auto found = uniqueLookup.find(key);
        if (found == uniqueLookup.end())
        {
            const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
            uniqueLookup.emplace(key, uniqueIndex);
            result.uniquePositions.push_back(vertex);
            result.renderToUnique.push_back(uniqueIndex);
        }
        else
        {
            result.renderToUnique.push_back(found->second);
        }
    }

    return result;
}

static std::vector<float3> GetMeshRenderVerticesForDeform(
    const std::shared_ptr<MeshInfo>& mesh,
    const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error(std::string(caller) + ": mesh has no buffer group");
    if (mesh->totalVertices == 0)
        return {};

    const auto& positions = mesh->buffers->positionData;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + size_t(mesh->totalVertices);
    if (positions.size() < end)
        throw std::runtime_error(std::string(caller) + ": CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    return std::vector<float3>(positions.begin() + begin, positions.begin() + end);
}

static const std::vector<uint32_t>* GetMeshSourcePositionIndicesForDeform(
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount)
{
    auto meshEx = std::dynamic_pointer_cast<MeshInfoEx>(mesh);
    if (!meshEx || meshEx->DeformationSourcePositionIndices.size() != renderVertexCount)
        return nullptr;

    return &meshEx->DeformationSourcePositionIndices;
}

std::vector<float3> Sample::GetMeshVertices(const std::shared_ptr<MeshInfo>& mesh) const
{
    std::vector<float3> renderVertices = GetMeshRenderVerticesForDeform(mesh, "GetMeshVertices");
    return BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size())).uniquePositions;
}

static std::shared_ptr<MeshInfo> GetMeshFromSceneNodeForDeform(
    const std::shared_ptr<SceneGraphNode>& node,
    const char* caller)
{
    if (!node)
        throw std::runtime_error(std::string(caller) + ": node is null");

    auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(node->GetLeaf());
    if (!meshInstance || !meshInstance->GetMesh())
        throw std::runtime_error(std::string(caller) + ": node is not a mesh instance");

    return meshInstance->GetMesh();
}

static std::shared_ptr<SceneGraphNode> FindUniqueMeshInstanceNodeForDeform(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!scene || !scene->GetSceneGraph())
        throw std::runtime_error(std::string(caller) + ": no scene is loaded");

    std::shared_ptr<SceneGraphNode> result;
    size_t instanceCount = 0;

    for (const auto& instance : scene->GetSceneGraph()->GetMeshInstances())
    {
        if (!instance || instance->GetMesh() != mesh)
            continue;

        if (auto node = instance->GetNodeSharedPtr())
        {
            result = node;
            ++instanceCount;
        }
    }

    if (instanceCount == 0)
        throw std::runtime_error(std::string(caller) + ": mesh has no scene instance");
    if (instanceCount > 1)
        throw std::runtime_error(std::string(caller) + ": mesh has multiple scene instances; pass a SceneNode instead");

    return result;
}

std::vector<float3> Sample::GetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_scene, mesh, "GetMeshVerticesWorld");
    return GetMeshVerticesWorld(node);
}

std::vector<float3> Sample::GetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node)
{
    if (!m_scene)
        throw std::runtime_error("GetMeshVerticesWorld: no scene is loaded");

    m_scene->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "GetMeshVerticesWorld");
    std::vector<float3> vertices = GetMeshVertices(mesh);

    const affine3 localToWorld = node->GetLocalToWorldTransformFloat();
    for (float3& vertex : vertices)
        vertex = localToWorld.transformPoint(vertex);

    return vertices;
}

void Sample::SetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_scene, mesh, "SetMeshVerticesWorld");
    SetMeshVerticesWorld(node, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void Sample::SetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    if (!m_scene)
        throw std::runtime_error("SetMeshVerticesWorld: no scene is loaded");

    m_scene->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "SetMeshVerticesWorld");
    const affine3 worldToLocal = affine3(inverse(node->GetLocalToWorldTransform()));

    std::vector<float3> objectVertices;
    objectVertices.reserve(vertices.size());
    for (const float3& vertex : vertices)
    {
        const float3 objectVertex = worldToLocal.transformPoint(vertex);
        if (!donut::math::all(donut::math::isfinite(objectVertex)))
            throw std::runtime_error("SetMeshVerticesWorld: world-to-object transform produced a non-finite vertex");
        objectVertices.push_back(objectVertex);
    }

    SetMeshVertices(mesh, objectVertices, recomputeNormals, rebuildAccelerationStructure);
}

static float3 NormalizeOrFallbackForDeform(const float3& v, const float3& fallback)
{
    const float len2 = dot(v, v);
    if (len2 <= 1e-20f || !std::isfinite(len2))
        return fallback;
    return v * (1.0f / std::sqrt(len2));
}

static void RecomputeMeshNormalsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);

    if (buffers.normalData.size() < vertexEnd || buffers.indexData.empty())
        return;

    std::vector<float3> renderVertices(buffers.positionData.begin() + vertexBegin, buffers.positionData.begin() + vertexEnd);
    UniquePositionMapForDeform uniqueMap = BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size()));

    std::vector<float3> accumulated(uniqueMap.uniquePositions.size(), float3(0.0f));

    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry || geometry->type != MeshGeometryPrimitiveType::Triangles)
            continue;

        const size_t indexBegin = size_t(mesh->indexOffset) + size_t(geometry->indexOffsetInMesh);
        const size_t indexEnd = indexBegin + size_t(geometry->numIndices);
        if (buffers.indexData.size() < indexEnd)
            continue;

        for (size_t i = indexBegin; i + 2 < indexEnd; i += 3)
        {
            uint32_t local[3] = {
                buffers.indexData[i + 0],
                buffers.indexData[i + 1],
                buffers.indexData[i + 2]
            };

            const uint32_t maxIndex = std::max(local[0], std::max(local[1], local[2]));
            if (maxIndex < geometry->numVertices)
            {
                local[0] += geometry->vertexOffsetInMesh;
                local[1] += geometry->vertexOffsetInMesh;
                local[2] += geometry->vertexOffsetInMesh;
            }

            if (local[0] >= mesh->totalVertices || local[1] >= mesh->totalVertices || local[2] >= mesh->totalVertices)
                continue;

            const float3& p0 = buffers.positionData[vertexBegin + local[0]];
            const float3& p1 = buffers.positionData[vertexBegin + local[1]];
            const float3& p2 = buffers.positionData[vertexBegin + local[2]];
            const float3 faceNormal = cross(p1 - p0, p2 - p0);
            if (!std::isfinite(dot(faceNormal, faceNormal)))
                continue;

            accumulated[uniqueMap.renderToUnique[local[0]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[1]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[2]]] += faceNormal;
        }
    }

    for (uint32_t i = 0; i < mesh->totalVertices; ++i)
    {
        const float3 normal = NormalizeOrFallbackForDeform(accumulated[uniqueMap.renderToUnique[i]], float3(0.0f, 1.0f, 0.0f));
        buffers.normalData[vertexBegin + i] = vectorToSnorm8(normal);
    }
}

static void UpdateMeshBoundsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);
    if (buffers.positionData.size() < vertexEnd)
        return;

    mesh->objectSpaceBounds = box3::empty();
    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry)
            continue;

        box3 bounds = box3::empty();
        const size_t geomBegin = vertexBegin + size_t(geometry->vertexOffsetInMesh);
        const size_t geomEnd = std::min(vertexEnd, geomBegin + size_t(geometry->numVertices));
        for (size_t i = geomBegin; i < geomEnd; ++i)
            bounds |= buffers.positionData[i];

        geometry->objectSpaceBounds = bounds;
        mesh->objectSpaceBounds |= bounds;
    }
}

static bool BufferRangeContainsBytes(const nvrhi::BufferRange& range, uint64_t relativeOffset, uint64_t byteSize)
{
    return range.byteSize != 0 && relativeOffset <= range.byteSize && byteSize <= range.byteSize - relativeOffset;
}

static nvrhi::ResourceStates GetMeshVertexBufferReadyState(const nvrhi::BufferDesc& desc)
{
    nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
    if (desc.isAccelStructBuildInput)
        state = state | nvrhi::ResourceStates::AccelStructBuildInput;
    return state;
}

static bool UploadMeshDeformationToGpu(
    nvrhi::IDevice* device,
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount,
    const std::vector<float3>* previousRenderVertices,
    bool uploadNormals)
{
    if (!device || !mesh || !mesh->buffers)
        return false;

    auto& buffers = *mesh->buffers;
    if (!buffers.vertexBuffer)
        return false;

    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertexCount;
    if (buffers.positionData.size() < end)
        return false;

    const nvrhi::BufferRange& positionRange = buffers.getVertexBufferRange(VertexAttribute::Position);
    const uint64_t positionOffset = uint64_t(begin) * sizeof(float3);
    const uint64_t positionBytes = uint64_t(renderVertexCount) * sizeof(float3);
    if (!BufferRangeContainsBytes(positionRange, positionOffset, positionBytes))
        return false;

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(
        buffers.vertexBuffer,
        buffers.positionData.data() + begin,
        positionBytes,
        positionRange.byteOffset + positionOffset);

    if (previousRenderVertices && previousRenderVertices->size() == renderVertexCount)
    {
        const nvrhi::BufferRange& prevPositionRange = buffers.getVertexBufferRange(VertexAttribute::PrevPosition);
        if (BufferRangeContainsBytes(prevPositionRange, positionOffset, positionBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                previousRenderVertices->data(),
                positionBytes,
                prevPositionRange.byteOffset + positionOffset);
        }
    }

    if (uploadNormals && buffers.normalData.size() >= end)
    {
        const nvrhi::BufferRange& normalRange = buffers.getVertexBufferRange(VertexAttribute::Normal);
        const uint64_t normalOffset = uint64_t(begin) * sizeof(uint32_t);
        const uint64_t normalBytes = uint64_t(renderVertexCount) * sizeof(uint32_t);
        if (BufferRangeContainsBytes(normalRange, normalOffset, normalBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                buffers.normalData.data() + begin,
                normalBytes,
                normalRange.byteOffset + normalOffset);
        }
    }

    commandList->setBufferState(buffers.vertexBuffer, GetMeshVertexBufferReadyState(buffers.vertexBuffer->getDesc()));
    commandList->close();
    device->executeCommandList(commandList);
    return true;
}

void Sample::SetMeshVertices(const std::shared_ptr<MeshInfo>& mesh,
                             const std::vector<float3>& vertices,
                             bool recomputeNormals,
                             bool rebuildAccelerationStructure)
{
    if (!mesh)
        throw std::runtime_error("SetMeshVertices: mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error("SetMeshVertices: mesh has no buffer group");

    std::vector<float3> renderVertices = GetMeshRenderVerticesForDeform(mesh, "SetMeshVertices");
    UniquePositionMapForDeform uniqueMap = BuildUniquePositionMapForDeform(
        renderVertices,
        GetMeshSourcePositionIndicesForDeform(mesh, renderVertices.size()));
    if (vertices.size() != uniqueMap.uniquePositions.size())
    {
        throw std::runtime_error(
            "SetMeshVertices: vertex count must match get_mesh_vertices(...) length");
    }

    for (size_t i = 0; i < renderVertices.size(); ++i)
        renderVertices[i] = vertices[uniqueMap.renderToUnique[i]];

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertices.size();
    if (buffers.positionData.size() < end)
        throw std::runtime_error("SetMeshVertices: CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    const std::vector<float3> previousRenderVertices(buffers.positionData.begin() + begin, buffers.positionData.begin() + end);
    std::copy(renderVertices.begin(), renderVertices.end(), buffers.positionData.begin() + begin);
    UpdateMeshBoundsFromPositions(mesh);

    if (recomputeNormals)
        RecomputeMeshNormalsFromPositions(mesh);

    const bool uploadedToExistingGpuBuffer = UploadMeshDeformationToGpu(
        GetDevice(),
        mesh,
        renderVertices.size(),
        &previousRenderVertices,
        recomputeNormals);

    if (!uploadedToExistingGpuBuffer)
    {
        GetDevice()->waitForIdle();

        if (buffers.vertexBuffer)
        {
            buffers.vertexBuffer = nullptr;
            buffers.vertexBufferDescriptor.reset();
            buffers.vertexBufferRanges.fill(nvrhi::BufferRange());
        }

        std::vector<std::shared_ptr<SceneGraphNode>> affectedNodes;
        if (m_scene && m_scene->GetSceneGraph())
        {
            for (const auto& instance : m_scene->GetSceneGraph()->GetMeshInstances())
            {
                if (instance && instance->GetMesh() == mesh)
                {
                    if (auto node = instance->GetNodeSharedPtr())
                        affectedNodes.push_back(node);
                }
            }
        }

        for (const auto& node : affectedNodes)
        {
            if (node && node->GetLeaf())
            {
                auto leaf = node->GetLeaf();
                node->SetLeaf(leaf);
            }
        }

        if (m_scene)
            m_scene->FinishedLoading(GetFrameIndex());
    }

    if (rebuildAccelerationStructure)
        RequestMeshAccelRebuild(mesh);
    else
        m_ui.ResetAccumulation = true;
}

void Sample::PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants)
{
    //m_commandList->beginMarker("MainRendering"); <- removed (for now) since added hierarchy reduces readability

    bool useStablePlanes = m_ui.RealtimeMode;

    nvrhi::rt::State state;

    nvrhi::rt::DispatchRaysArguments args;
    nvrhi::Viewport viewport = m_view->GetViewport();
    uint32_t width = (uint32_t)(viewport.maxX - viewport.minX);
    uint32_t height = (uint32_t)(viewport.maxY - viewport.minY);
    args.width = width;
    args.height = height;

    // default miniConstants
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    if (useStablePlanes)
    {
        {
            RAII_SCOPE(m_commandList->beginMarker("PathTracePrePass"); , m_commandList->endMarker(); );

            m_commandList->setTextureState(m_renderTargets->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->ScreenMotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Throughput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            state.shaderTable = m_ptPipelineBuildStablePlanes->GetShaderTable();
            state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };
            m_commandList->setRayTracingState(state);
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
            m_commandList->dispatchRays(args);
            m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->ScreenMotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Throughput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        {
            RAII_SCOPE(m_commandList->beginMarker("VBufferExport"); , m_commandList->endMarker(); );

            nvrhi::ComputeState state;
		    state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };
            state.pipeline = m_exportVBufferPSO;
            m_commandList->setComputeState(state);

		    const dm::uint2 dispatchSize = { (width + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM, (height + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM };
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
		    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
        }
    }
    else
    {

    }

    // In realtime mode, ScreenMotionVectors reference mode ScreenMotionVectors should be 0
    m_lightsBaker->UpdateEnd(m_commandList, *m_bindingCache, m_scene, m_materialsBaker, m_ommBaker, m_subInstanceBuffer, m_renderTargets->Depth, m_renderTargets->ScreenMotionVectors);  // <- in the future this will provide motion vectors except in case of reference mode

    {
        RAII_SCOPE( m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

        state.shaderTable = ((useStablePlanes) ? (m_ptPipelineFillStablePlanes) : (m_ptPipelineReference))->GetShaderTable();
        state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };

        for (uint subSampleIndex = 0; subSampleIndex < m_ui.ActualSamplesPerPixel(); subSampleIndex++)
        {
            // required to avoid race conditions in back to back dispatchRays
            m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

            m_commandList->setRayTracingState(state);

            // tell path tracer which subSampleIndex we're processing
            SampleMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };//     <- use subSampleIndex to try to figure out why we're losing radiance - is the first one what's left, or the last one?
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

            m_commandList->dispatchRays(args);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    // this is a performance optimization where final 2 passes from ReSTIR DI and ReSTIR GI are combined to avoid loading GBuffer twice
    static bool enableFusedDIGIFinal = true;
    bool useFusedDIGIFinal = m_ui.ActualUseReSTIRDI() && m_ui.ActualUseReSTIRGI() && enableFusedDIGIFinal;

    if (m_ui.ActualUseRTXDIPasses())
    {
        RAII_SCOPE( m_commandList->beginMarker("RTXDI");, m_commandList->endMarker(); );

        // this does all ReSTIR DI magic including applying the final sample into correct radiance buffer (depending on denoiser state)
        if (m_ui.ActualUseReSTIRDI())
            m_rtxdiPass->Execute(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (m_ui.ActualUseReSTIRGI())
            m_rtxdiPass->ExecuteGI(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (useFusedDIGIFinal)
            m_rtxdiPass->ExecuteFusedDIGIFinal(m_commandList, m_bindingSet);

        if (m_ui.ActualUseReSTIRPT())
            m_rtxdiPass->ExecutePT(m_commandList, m_bindingSet);
    }

    {
        RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

        m_denoisingGuidesBaker->DenoiseSpecHitT(m_commandList, m_bindingSet);
        m_denoisingGuidesBaker->ComputeAvgLayerRadiance(m_commandList, m_bindingSet);

        if (m_ui.DebugView != DebugViewType::Disabled)
            m_denoisingGuidesBaker->RenderDebugViz(m_commandList, m_ui.DebugView, m_bindingSet);
    }

    if (useStablePlanes && (m_ui.DebugView > DebugViewType::Disabled && m_ui.DebugView <= DebugViewType::StablePlane_SpecRadiance || m_ui.DebugView == DebugViewType::StableRadiance) )
    {
        m_commandList->beginMarker("StablePlanesDebugViz");
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::StablePlanesDebugViz, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

    }
}

void Sample::Denoise(nvrhi::IFramebuffer* framebuffer)
{
    if( !m_ui.ActualUseStandaloneDenoiser() )
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] == nullptr)
        {
            nrd::Denoiser denoiserMethod = m_ui.NRDMethod == NrdConfig::DenoiserMethod::REBLUR ?
                nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

            m_nrd[i] = std::make_unique<NrdIntegration>(GetDevice(), denoiserMethod);
            m_nrd[i]->Initialize(m_renderSize.x, m_renderSize.y, *m_shaderFactory);
        }
    }

    //const auto& fbinfo = framebuffer->getFramebufferInfo();
    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" }; assert( std::size(m_nrd) <= std::size(passNames) );

    bool nrdUseRelax = m_ui.NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    PostProcess::ComputePassType preparePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    PostProcess::ComputePassType mergePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    bool resetHistory = m_ui.ResetRealtimeCaches;

    int maxPassCount = std::min(m_ui.StablePlanesActiveCount, (int)std::size(m_nrd));
    bool initWithStableRadiance = true;
    for (int pass = maxPassCount-1; pass >= 0; pass--)
    {
        m_commandList->beginMarker(passNames[pass]);

        SampleMiniConstants miniConstants = { uint4((uint)pass, initWithStableRadiance?1:0, 0, 0) };
        initWithStableRadiance = false;

        // Direct inputs to denoiser are reused between passes; there's redundant copies but it makes interfacing simpler
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_commandList->beginMarker("PrepareInputs");
        m_postProcess->Apply(m_commandList, preparePassType, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

        const float timeDeltaBetweenFrames = m_cmdLine.noWindow ? 1.f/60.f : -1.f; // if we're rendering without a window we set a fix timeDeltaBetweenFrames to ensure that output is deterministic
        bool enableValidation = m_ui.DebugView == DebugViewType::StablePlane_DenoiserValidation;
        if (nrdUseRelax)
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_view, *m_viewPrevious, GetFrameIndex(), m_ui.NRDDisocclusionThreshold, m_ui.NRDDisocclusionThresholdAlternate, m_ui.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_ui.RelaxSettings);
        }
        else
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_view, *m_viewPrevious, GetFrameIndex(), m_ui.NRDDisocclusionThreshold, m_ui.NRDDisocclusionThresholdAlternate, m_ui.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_ui.ReblurSettings);
        }

        m_commandList->beginMarker("MergeOutputs");
        m_postProcess->Apply(m_commandList, mergePassType, pass, m_constantBuffer, miniConstants, m_renderTargets->OutputColor, *m_renderTargets, nullptr);
        m_commandList->endMarker();

        m_commandList->endMarker();
    }
}

#if RTXPT_WITH_NATIVE_DLSS
bool Sample::EvaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_ui.RealtimeAA == 3;
    if (useRayReconstruction && !m_nativeDLSS->IsRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_nativeDLSS->IsDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(m_commandList->beginMarker("DLSSRR_PrepareInputs");, m_commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
    }

    donut::render::DLSS::InitParameters initParams;
    initParams.inputWidth = m_renderSize.x;
    initParams.inputHeight = m_renderSize.y;
    initParams.outputWidth = m_displaySize.x;
    initParams.outputHeight = m_displaySize.y;
    initParams.useLinearDepth = false;
    initParams.useAutoExposure = true;
    initParams.useRayReconstruction = useRayReconstruction;

    m_nativeDLSS->Init(initParams);

    const bool initialized = useRayReconstruction
        ? m_nativeDLSS->IsRayReconstructionInitialized()
        : m_nativeDLSS->IsDlssInitialized();
    if (!initialized)
        return false;

    donut::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_renderTargets->OutputColor;
    evaluateParams.outputColorTexture = m_renderTargets->ProcessedOutputColor;
    evaluateParams.depthTexture = m_renderTargets->Depth;
    evaluateParams.motionVectorsTexture = m_renderTargets->ScreenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_renderSize.y);
    evaluateParams.resetHistory = reset || m_ui.ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->RRDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->RRSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->RRNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->Evaluate(m_commandList, evaluateParams, *m_view);
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            log::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_renderSize.x, m_renderSize.y, m_displaySize.x, m_displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif

void Sample::PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    if (m_ui.RealtimeMode)
    {
        if (m_ui.RealtimeAA == 0)
        {
            // TODO: Remove Redundant copy for non AA case
            m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_renderTargets->OutputColor, nvrhi::TextureSlice());
        }
        else if (m_ui.RealtimeAA == 1 && m_temporalAntiAliasingPass != nullptr )
        {
            const bool stochasticSplats = m_ui.EnableGaussianSplats && m_ui.GaussianSplatSortingMode == 1;
            const bool stochasticReset = stochasticSplats && (reset || m_ui.ResetAccumulation || m_ui.ResetRealtimeCaches || m_gaussianSplatTemporalReset);
            if (stochasticReset)
                m_gaussianSplatTemporalSampleIndex = 0;

            bool previousViewValid = !reset && !stochasticReset && (GetFrameIndex() != 0);
            donut::render::TemporalAntiAliasingParameters taaParams = m_ui.TemporalAntiAliasingParams;
            if (stochasticSplats)
            {
                taaParams.enableHistoryClamping = false;
                taaParams.useHistoryClampRelax = false;
                taaParams.newFrameWeight = 1.0f / float(m_gaussianSplatTemporalSampleIndex + 1);
            }

            m_commandList->beginMarker("TAA");

            m_temporalAntiAliasingPass->TemporalResolve(m_commandList, taaParams, previousViewValid, *m_view, *m_view);

            m_commandList->endMarker();

            if (stochasticSplats)
            {
                m_gaussianSplatTemporalSampleIndex = std::min(m_gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
                m_gaussianSplatTemporalReset = false;
            }
        }

#if DONUT_WITH_STREAMLINE
        const bool useStreamlineThisFrame = !GetDeviceManager()->GetDeviceParams().headlessDevice;
        if (useStreamlineThisFrame)
        {
        // SET STREAMLINE CONSTANTS
        {
            // This section of code updates the streamline constants every frame. Regardless of whether we are utilising the streamline plugins, as long as streamline is in use, we must set its constants.
            affine3 viewReprojection = m_view->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix() * m_viewPrevious->GetViewMatrix();
            float4x4 reprojectionMatrix = inverse(m_view->GetProjectionMatrix(false)) * affineToHomogeneous(viewReprojection) * m_viewPrevious->GetProjectionMatrix(false);
            float outputAspectRatio = m_displayAspectRatio; //windowViewport.width() / windowViewport.height();    // render and display outputs might not match in case of lower DLSS/etc resolution rounding!
            float4x4 projection = perspProjD3DStyleReverse(dm::radians(m_cameraVerticalFOV), outputAspectRatio, m_cameraZNear);

            //float2 jitterOffset = std::dynamic_pointer_cast<PlanarView, IView>(m_view)->GetPixelOffset();
            float2 jitterOffset = ComputeCameraJitter(m_sampleIndex);

            StreamlineInterface::Constants slConstants = {};
            slConstants.cameraAspectRatio = outputAspectRatio;
            slConstants.cameraFOV = dm::radians(m_cameraVerticalFOV);
            slConstants.cameraFar = m_cameraZFar;
            slConstants.cameraMotionIncluded = true;
            slConstants.cameraNear = m_cameraZNear;
            slConstants.cameraPinholeOffset = { 0.f, 0.f };
            slConstants.cameraPos = m_camera.GetPosition();
            slConstants.cameraFwd = m_camera.GetDir();
            slConstants.cameraUp = m_camera.GetUp();
            slConstants.cameraRight = normalize(cross(m_camera.GetDir(), m_camera.GetUp()));
            slConstants.cameraViewToClip = projection;
            slConstants.clipToCameraView = inverse(projection);
            slConstants.clipToPrevClip = reprojectionMatrix;
            slConstants.depthInverted = m_view->IsReverseDepth() ? true : false;
            slConstants.jitterOffset = jitterOffset;
            slConstants.mvecScale = { 1.0f / m_renderSize.x , 1.0f / m_renderSize.y }; // This are scale factors used to normalize mvec (to -1,1) and donut has mvec in pixel space
            slConstants.prevClipToClip = inverse(reprojectionMatrix);
            slConstants.reset = reset;
            slConstants.motionVectors3D = false;
            slConstants.motionVectorsInvalidValue = FLT_MIN;

            GetDeviceManager()->GetStreamline().SetConstants(slConstants);

            if (m_ui.RealtimeAA == 3) // DLSS-RR
            {
                // only used if hitT (hit distance) codepath is used
                m_lastDLSSRROptions.worldToCameraView = dm::affineToHomogeneous(m_view->GetViewMatrix());
                m_lastDLSSRROptions.cameraViewToWorld = dm::affineToHomogeneous(m_view->GetInverseViewMatrix());
                
                GetDeviceManager()->GetStreamline().SetDLSSRROptions(m_lastDLSSRROptions);
            }
        }

        m_commandList->commitBarriers();

        // TAG STREAMLINE RESOURCES
        GetDeviceManager()->GetStreamline().TagResourcesGeneral(m_commandList,
            m_view->GetChildView(ViewType::PLANAR, 0),
            m_renderTargets->ScreenMotionVectors,
            m_renderTargets->Depth,
            m_renderTargets->PreUIColor);

        // TAG STREAMLINE RESOURCES
        GetDeviceManager()->GetStreamline().TagResourcesDLSSNIS(m_commandList,
            m_view->GetChildView(ViewType::PLANAR, 0),
            m_renderTargets->ProcessedOutputColor,
            m_renderTargets->OutputColor);

        if (m_ui.RealtimeAA == 2)
        {
            RAII_SCOPE(m_commandList->beginMarker("DLSS"); , m_commandList->endMarker(); );

            GetDeviceManager()->GetStreamline().EvaluateDLSS(m_commandList);
            m_commandList->clearState();
        }
        }
        if (useStreamlineThisFrame && m_ui.RealtimeAA == 3)
        {
            RAII_SCOPE(m_commandList->beginMarker("DLSS-RR");, m_commandList->endMarker(); );

            // Direct inputs to denoiser are reused between passes; there's redundant copies but it makes interfacing simpler
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
            m_commandList->beginMarker("DLSSRR_PrepareInputs");
            m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
            m_commandList->endMarker();

            static bool useSpecHitT = false; // it's buggy
            //if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_G))
            //    gmgkmgk = !gmgkmgk;

#if 0
            GetDeviceManager()->GetStreamline().TagResourcesDLSSRR(m_commandList,
                m_view->GetChildView(ViewType::PLANAR, 0),
                (int2)m_renderSize,
                (int2)m_displaySize,
                m_renderTargets->ProcessedOutputColor,  //nvrhi::ITexture* outputColor
                m_renderTargets->OutputColor,           // nvrhi::ITexture* inputColor
                m_renderTargets->RRDiffuseAlbedo,
                m_renderTargets->RRSpecAlbedo,
                m_renderTargets->RRNormalsAndRoughness,
                nullptr,
                (useSpecHitT) ? (m_renderTargets->SpecularHitT) : (nullptr),
                (useSpecHitT) ? (nullptr) : (m_renderTargets->RRSpecMotionVectors),
                m_renderTargets->RRTransparencyLayer
#else
            GetDeviceManager()->GetStreamline().TagResourcesDLSSRR(m_commandList,
                m_view->GetChildView(ViewType::PLANAR, 0),
                (int2)m_renderSize,
                (int2)m_displaySize,
                m_renderTargets->OutputColor,           // nvrhi::ITexture* inputColor
                m_renderTargets->RRDiffuseAlbedo,
                m_renderTargets->RRSpecAlbedo,
                m_renderTargets->RRNormalsAndRoughness,
                nullptr,
                (useSpecHitT)?(m_renderTargets->SpecularHitT):(nullptr),
                (useSpecHitT)?(nullptr):(m_renderTargets->RRSpecMotionVectors),
                m_renderTargets->ProcessedOutputColor   //nvrhi::ITexture* outputColor
            );
#endif

            m_commandList->commitBarriers();

            GetDeviceManager()->GetStreamline().EvaluateDLSSRR(m_commandList);
            m_commandList->clearState();
        }
        else if ( !m_ui.ActualUseStandaloneDenoiser() )
        {
            // If all denoisers disabled, this is a pass-through that just merges and outputs noisy data
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
            m_commandList->beginMarker("NoDenoiserFinalMerge");
            m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::NoDenoiserFinalMerge, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
            m_commandList->endMarker();
        }
#endif

#if RTXPT_WITH_NATIVE_DLSS
        bool nativeDLSSEvaluated = false;
        if (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3)
            nativeDLSSEvaluated = EvaluateNativeDLSS(reset);

        if (!nativeDLSSEvaluated && (m_ui.RealtimeAA == 2 || m_ui.RealtimeAA == 3))
        {
            if (m_ui.ActualUseStandaloneDenoiser())
            {
                m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_renderTargets->OutputColor, nvrhi::TextureSlice());
            }
            else
            {
                SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
                m_commandList->beginMarker("NoDenoiserFinalMerge");
                m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::NoDenoiserFinalMerge, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
                m_commandList->endMarker();
            }
        }
#endif
    }
    else
    {
        // Reference mode - run the accumulation pass.
        // If the sample count has reached the target, just keep copying the accumulated output.
        // If m_accumulationSampleIndex is negative - that's warm-up, just display.
        const float accumulationWeight = (m_accumulationSampleIndex < m_ui.AccumulationTarget)?(1.f / float(max(0, m_accumulationSampleIndex) + 1)):(0.0f);

        m_accumulationPass->Render(m_commandList, *m_view, *m_view, accumulationWeight);
    }

}

static float ReadR11G11B10FloatChannel(uint32_t packed, uint32_t channel)
{
    uint16_t halfBits = 0;
    switch (channel)
    {
    case 0: halfBits = uint16_t((packed << 4) & 0x7FF0); break;
    case 1: halfBits = uint16_t((packed >> 7) & 0x7FF0); break;
    default: halfBits = uint16_t((packed >> 17) & 0x7FE0); break;
    }

    const float value = Float16ToFloat32(float16_t{ halfBits });
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

static nvrhi::TextureDesc MakeReadbackTextureDesc(nvrhi::TextureDesc desc, const char* debugName)
{
    desc.debugName = debugName;
    desc.isRenderTarget = false;
    desc.isUAV = false;
    desc.isTypeless = false;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    return desc;
}

static void ReadR11G11B10Float3Staging(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture, uint32_t width, uint32_t height, std::vector<float>& output)
{
    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
        return;

    output.resize(size_t(width) * size_t(height) * 3);
    for (uint32_t y = 0; y < height; y++)
    {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            const uint32_t packed = row[x];
            output[targetOffset + 0] = ReadR11G11B10FloatChannel(packed, 0);
            output[targetOffset + 1] = ReadR11G11B10FloatChannel(packed, 1);
            output[targetOffset + 2] = ReadR11G11B10FloatChannel(packed, 2);
        }
    }

    device->unmapStagingTexture(stagingTexture);
}

static void ReadRGBA16Float3Staging(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture, uint32_t width, uint32_t height, std::vector<float>& output)
{
    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
        return;

    output.resize(size_t(width) * size_t(height) * 3);
    for (uint32_t y = 0; y < height; y++)
    {
        const float16_t4* row = reinterpret_cast<const float16_t4*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const float4 value = Float16ToFloat32x4(row[x]);
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            output[targetOffset + 0] = std::isfinite(value.x) ? std::clamp(value.x, -1.0f, 1.0f) : 0.0f;
            output[targetOffset + 1] = std::isfinite(value.y) ? std::clamp(value.y, -1.0f, 1.0f) : 0.0f;
            output[targetOffset + 2] = std::isfinite(value.z) ? std::clamp(value.z, -1.0f, 1.0f) : 1.0f;
        }
    }

    device->unmapStagingTexture(stagingTexture);
}

void Sample::ResetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->Reset();
}

void Sample::ApplyReferenceOIDN()
{
    if (m_ui.RealtimeMode || !m_ui.ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if RTXPT_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_ui.AccumulationTarget;
    if (!accumulationReady)
        return;

    if (m_oidnDenoiserFailed)
        return;

    const nvrhi::TextureDesc processedDesc = m_renderTargets->ProcessedOutputColor->getDesc();
    if (m_oidnDenoisedOutput == nullptr ||
        m_oidnDenoisedOutput->getDesc().width != processedDesc.width ||
        m_oidnDenoisedOutput->getDesc().height != processedDesc.height ||
        m_oidnDenoisedOutput->getDesc().format != processedDesc.format)
    {
        nvrhi::TextureDesc oidnOutputDesc = processedDesc;
        oidnOutputDesc.debugName = "ReferenceOIDNDenoisedOutput";
        oidnOutputDesc.initialState = nvrhi::ResourceStates::CopySource;
        oidnOutputDesc.keepInitialState = true;
        m_oidnDenoisedOutput = GetDevice()->createTexture(oidnOutputDesc);
        m_oidnDenoisedOutputValid = false;
    }

    if (m_oidnDenoisedOutputValid)
    {
        m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
        return;
    }

    nvrhi::ITexture* sourceTexture = m_renderTargets->AccumulatedRadiance;
    nvrhi::TextureDesc sourceDesc = sourceTexture->getDesc();
    if (sourceDesc.format != nvrhi::Format::RGBA32_FLOAT)
    {
        donut::log::warning("OIDN reference denoiser expected RGBA32_FLOAT accumulation buffer, got %s.", nvrhi::utils::FormatToString(sourceDesc.format));
        m_oidnDenoiserFailed = true;
        return;
    }

    const uint32_t width = sourceDesc.width;
    const uint32_t height = sourceDesc.height;

    OidnDenoiser::Options oidnOptions;
    oidnOptions.UseGPU = m_ui.ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_ui.ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_ui.ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_ui.ReferenceOIDNQuality, 0, 2));

    const bool requestAlbedoGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::Albedo || oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    const bool requestNormalGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    if (requestAlbedoGuide || requestNormalGuide)
    {
        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_commandList->beginMarker("OIDN_PrepareGuides");
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();
    }

    nvrhi::StagingTextureHandle stagingTexture = GetDevice()->createStagingTexture(
        MakeReadbackTextureDesc(sourceDesc, "ReferenceOIDN AccumulatedRadiance Readback"),
        nvrhi::CpuAccessMode::Read);
    if (stagingTexture == nullptr)
    {
        donut::log::warning("OIDN reference denoiser failed to create a staging texture.");
        m_oidnDenoiserFailed = true;
        return;
    }

    nvrhi::StagingTextureHandle albedoStagingTexture;
    nvrhi::StagingTextureHandle normalStagingTexture;
    if (requestAlbedoGuide && m_renderTargets->RRDiffuseAlbedo != nullptr)
    {
        albedoStagingTexture = GetDevice()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->RRDiffuseAlbedo->getDesc(), "ReferenceOIDN Albedo Readback"),
            nvrhi::CpuAccessMode::Read);
        if (albedoStagingTexture != nullptr)
            m_commandList->copyTexture(albedoStagingTexture, nvrhi::TextureSlice(), m_renderTargets->RRDiffuseAlbedo, nvrhi::TextureSlice());
    }
    if (requestNormalGuide && m_renderTargets->RRNormalsAndRoughness != nullptr)
    {
        normalStagingTexture = GetDevice()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->RRNormalsAndRoughness->getDesc(), "ReferenceOIDN Normal Readback"),
            nvrhi::CpuAccessMode::Read);
        if (normalStagingTexture != nullptr)
            m_commandList->copyTexture(normalStagingTexture, nvrhi::TextureSlice(), m_renderTargets->RRNormalsAndRoughness, nvrhi::TextureSlice());
    }

    m_commandList->copyTexture(stagingTexture, nvrhi::TextureSlice(), sourceTexture, nvrhi::TextureSlice());
    m_commandList->close();
    GetDevice()->executeCommandList(m_commandList);
    if (!GetDevice()->waitForIdle())
    {
        m_commandList->open();
        donut::log::warning("OIDN reference denoiser readback failed because the GPU device was lost.");
        m_oidnDenoiserFailed = true;
        return;
    }

    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(GetDevice()->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
    {
        m_commandList->open();
        donut::log::warning("OIDN reference denoiser failed to map the accumulation buffer.");
        m_oidnDenoiserFailed = true;
        return;
    }

    std::vector<float> inputRgb(size_t(width) * size_t(height) * 3);

    for (uint32_t y = 0; y < height; y++)
    {
        const float* row = reinterpret_cast<const float*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const size_t sourceOffset = size_t(x) * 4;
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            for (uint32_t channel = 0; channel < 3; channel++)
            {
                const float value = row[sourceOffset + channel];
                inputRgb[targetOffset + channel] = (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
            }
        }
    }

    GetDevice()->unmapStagingTexture(stagingTexture);

    std::vector<float> albedoRgb;
    std::vector<float> normalRgb;
    if (albedoStagingTexture != nullptr)
    {
        ReadR11G11B10Float3Staging(GetDevice(), albedoStagingTexture, width, height, albedoRgb);
        if (!albedoRgb.empty())
            oidnOptions.AlbedoRgb = albedoRgb.data();
    }
    if (normalStagingTexture != nullptr)
    {
        ReadRGBA16Float3Staging(GetDevice(), normalStagingTexture, width, height, normalRgb);
        if (!normalRgb.empty())
            oidnOptions.NormalRgb = normalRgb.data();
    }

    if (m_oidnDenoiser == nullptr)
        m_oidnDenoiser = std::make_unique<OidnDenoiser>();

    std::vector<float> outputRgb;
    const bool success = m_oidnDenoiser->Denoise(inputRgb.data(), width, height, oidnOptions, outputRgb);

    m_commandList->open();

    if (!success)
    {
        donut::log::warning("OIDN reference denoiser failed: %s", m_oidnDenoiser->GetLastError().c_str());
        m_oidnDenoiserFailed = true;
        return;
    }

    std::vector<float16_t4> outputHalf(size_t(width) * size_t(height));
    constexpr float maxHalf = 65504.0f;
    for (size_t pixel = 0; pixel < outputHalf.size(); pixel++)
    {
        const size_t rgbOffset = pixel * 3;
        const float r = std::clamp(std::isfinite(outputRgb[rgbOffset + 0]) ? outputRgb[rgbOffset + 0] : 0.0f, 0.0f, maxHalf);
        const float g = std::clamp(std::isfinite(outputRgb[rgbOffset + 1]) ? outputRgb[rgbOffset + 1] : 0.0f, 0.0f, maxHalf);
        const float b = std::clamp(std::isfinite(outputRgb[rgbOffset + 2]) ? outputRgb[rgbOffset + 2] : 0.0f, 0.0f, maxHalf);
        outputHalf[pixel] = Float32ToFloat16x4(float4(r, g, b, 1.0f));
    }

    m_commandList->writeTexture(m_oidnDenoisedOutput, 0, 0, outputHalf.data(), size_t(width) * sizeof(float16_t4));
    m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
    m_oidnDenoisedOutputValid = true;

    donut::log::info("OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->GetDeviceDescription().c_str(), width, height);
#else
    if (!m_oidnDenoiserFailed)
    {
        donut::log::warning("OIDN reference denoiser requested, but RTXPT_WITH_OIDN is disabled in this build.");
        m_oidnDenoiserFailed = true;
    }
#endif
}

void Sample::DenoisedScreenshot(nvrhi::ITexture * framebufferTexture) const
{
    std::string noisyImagePath = (app::GetDirectoryWithExecutable( ) / "photo.bmp").string();

    auto execute = [&](const std::string & dn = "OptiX")
    {
	    const auto p1 = std::chrono::system_clock::now();
		const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count());

        const std::string fileName = "photo-denoised_" + dn + "_" + timestamp + ".bmp";

        std::string denoisedImagePath = (app::GetDirectoryWithExecutable() / fileName).string();
        std::filesystem::path denoiserPath = GetLocalPath("Support/denoiser_"+dn) / "denoiser.exe";
        if (!std::filesystem::exists(denoiserPath))
        {
            donut::log::warning("External %s denoiser not found at '%s'.", dn.c_str(), denoiserPath.string().c_str());
            return;
        }

        if (!SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, noisyImagePath.c_str()))
        { assert(false); return; }

        std::string startCmd = "\"" + denoiserPath.string() + "\"" + " -hdr 0 -i \"" + noisyImagePath + "\"" " -o \"" + denoisedImagePath + "\"";
        auto [resNum, resString, errorString] =  SystemShell(startCmd.c_str());
        if (resString!="")
            donut::log::info("result: %s", resString.c_str());
        if (errorString != "")
            donut::log::info("error: %s", errorString.c_str());

        std::string viewCmd = "\"" + denoisedImagePath + "\"";
        SystemShell(viewCmd.c_str(), true);
    };
    execute("OptiX");
    execute("OIDN");
}

donut::math::float2 Sample::ComputeCameraJitter(uint frameIndex)
{
    if (!m_ui.RealtimeMode || m_ui.RealtimeAA == 0 || m_temporalAntiAliasingPass == nullptr || m_ui.DbgFreezeRealtimeNoiseSeed)
        return dm::float2(0,0);

    // we currently use TAA for jitter even when it's not used itself
    return m_temporalAntiAliasingPass->GetCurrentPixelOffset();
}
