#include "PathTracerApp.h"
#include "caustica.h"

#include <render/Core/PathTracerPostProcess.h>

#include <core/path_utils.h>
#include <scene/scene_utils.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/BindingCache.h>
#include <render/Core/View.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/json.h>
#include <core/vfs/VFS.h>
#include <math/float.h>
#include <math/math.h>
#include <shaders/light_cb.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>
#include <rhi/common/misc.h>
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

#include <render/Core/PTPipelineBaker.h>
#include <render/Core/ComputePipelineBaker.h>

#include "render/Core/AccelerationStructureUtil.h"

#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>

#include <render/Passes/OMM/OmmBaker.h>

#include "SampleCommon/LocalConfig.h"
#include "SampleCommon/SampleBaseApp.h"
#include "SampleCommon/CaptureScriptManager.h"
#include <render/Passes/Debug/Korgi.h>

#include <render/GPUSort/GPUSort.h>

#include <render/Passes/Debug/ZoomTool.h>

#include <render/Passes/PostProcess/DenoisingGuidesBaker.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>

#include <assets/loader/GltfImporter.h>
#include <assets/loader/ObjImporter.h>
#include <scene/SceneGraph.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include "SampleGame/GameScene.h"

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

#include <fstream>
#include <iostream>

#include <thread>

namespace
{
    constexpr const char* c_InlineSceneSentinel = "__CAUSTICA_INLINE_SCENE_JSON__";

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

#if CAUSTICA_WITH_NATIVE_DLSS
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

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
// Required for Agility SDK on Windows 10. Setup 1.c. 2.a.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = CAUSTICA_D3D_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

const char* g_windowTitle = "caustica";

const float c_envMapRadianceScale = 1.0f / 4.0f; // used to make input 32bit float radiance fit into 16bit float range that baker supports; going lower than 1/4 causes issues with current BC6U compression algorithm when used

static FPSLimiter g_FPSLimiter;

PathTracerApp::PathTracerApp(caustica::GpuDevice& deviceManager,
    const CommandLineOptions& cmdLine,
    SampleUIData& ui)
    : caustica::SceneRender(&deviceManager )
    , m_cmdLine(cmdLine)
    , m_settings(ui)
    , m_editor(ui)
    , m_ui(ui)
    , m_renderCore(deviceManager.GetDevice())
{
    m_progressLoading.Start("Initializing...");
    m_progressLoading.Set(50);

    m_renderCore.camera().camera().SetRotateSpeed(.003f);

#if CAUSTICA_WITH_STREAMLINE
    if (!GetGpuDevice()->GetDeviceParams().headlessDevice)
    {
        m_settings.IsDLSSSuported = GetGpuDevice()->GetStreamline().IsDLSSAvailable();
        m_settings.IsDLSSFGSupported = GetGpuDevice()->GetStreamline().IsDLSSGAvailable();
        m_settings.IsReflexSupported = GetGpuDevice()->GetStreamline().IsReflexAvailable();
        m_settings.IsDLSSRRSupported = GetGpuDevice()->GetStreamline().IsDLSSRRAvailable();
    }
#endif

    RefreshEnvironmentMapMediaList();

    // Wire SceneLoader to the subclass virtual LoadScene().
    m_Loader.setLoadFunc([this](std::shared_ptr<caustica::IFileSystem> fs,
                                const std::filesystem::path& path)
    {
        return LoadScene(std::move(fs), path);
    });

    m_captureScriptManager = std::make_unique<CaptureScriptManager>(static_cast<Sample&>(*this), m_ui, m_cmdLine);

#if CAUSTICA_WITH_PYTHON
    // Embedded Python scripting host - we always create the wrapper but the
    // interpreter itself is initialized on demand the first time a script
    // gets queued.  This keeps cold-start overhead at zero when scripting is
    // unused even if the executable was built with CAUSTICA_WITH_PYTHON=ON.
    m_pythonScripting = std::make_unique<PythonScripting>(static_cast<Sample&>(*this));
#endif
}

PathTracerApp::~PathTracerApp()
{
#if CAUSTICA_WITH_PYTHON
    // Tear down the Python interpreter first so that any nb::class_<>-bound
    // C++ objects (materials, lights, ...) are released while their owning
    // C++ data is still alive.
    m_pythonScripting.reset();
#endif
}

void PathTracerApp::DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 )
{
    m_renderer->debugDrawLine(start, stop, col1, col2);
}

void PathTracerApp::Init(const std::string& preferredScene,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    m_shaderFactory = shaderFactory;

    m_CommonPasses = std::make_shared<caustica::CommonRenderPasses>(GetDevice(), m_shaderFactory);
    m_bindingCache = std::make_unique<caustica::BindingCache>(GetDevice());

#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = caustica::render::DLSS::Create(GetDevice(), *m_shaderFactory, caustica::GetDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
        caustica::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_settings.IsDLSSSuported ? "yes" : "no",
            m_settings.IsDLSSRRSupported ? "yes" : "no");
    }
    else
    {
        caustica::warning("Native NGX DLSS object was not created.");
    }
#endif

    m_settings.EnableGaussianSplats = true;
    m_settings.GaussianSplatDepthTest = m_cmdLine.GaussianSplatDepthTest;
    m_settings.GaussianSplatScale = m_cmdLine.GaussianSplatScale;
    m_settings.GaussianSplatAlphaScale = m_cmdLine.GaussianSplatAlphaScale;
    m_settings.GaussianSplatBrightness = m_cmdLine.GaussianSplatBrightness;
    m_settings.GaussianSplatAsEmitter = m_cmdLine.GaussianSplatAsEmitter;
    m_settings.GaussianSplatEmissionIntensity = m_cmdLine.GaussianSplatEmissionIntensity;
    m_settings.GaussianSplatEmissionMaxProxyCount = m_cmdLine.GaussianSplatEmissionMaxProxyCount;
    m_settings.GaussianSplatAlphaCullThreshold = m_cmdLine.GaussianSplatAlphaCullThreshold;
    
    m_sampleGame = std::make_unique<GameScene>(static_cast<Sample&>(*this), m_cmdLine);
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

    m_DescriptorTable = std::make_shared<caustica::DescriptorTableManager>(device, m_bindlessLayout);

    auto nativeFS = std::make_shared<caustica::NativeFileSystem>();
    m_TextureCache = std::make_shared<caustica::TextureCache>(device, nativeFS, m_DescriptorTable);

    m_sceneManager = std::make_unique<SceneManager>(
        *GetGpuDevice(), *m_shaderFactory, m_TextureCache, m_DescriptorTable);

    m_renderer = std::make_unique<Renderer>(static_cast<Sample&>(*this));

    memset( &m_feedbackData, 0, sizeof(DebugFeedbackStruct) * 1 );
    memset( &m_debugDeltaPathTree, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices );

    //Draw lines from the feedback buffer - this is old and should all be replaced by ShaderDebug
    {
        std::vector<ShaderMacro> drawLinesMacro = { ShaderMacro("DRAW_LINES_SHADERS_OLD", "1") };
        m_linesVertexShader = m_shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, nvrhi::ShaderType::Vertex);
        m_linesPixelShader = m_shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, nvrhi::ShaderType::Pixel);

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
            bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
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
            bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
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
            bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
            bufferDesc.structStride = sizeof(PathPayload);
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::Common;
            bufferDesc.debugName = "DebugDeltaPathTreeSearchStack";
            m_debugDeltaPathTreeSearchStack = device->createBuffer(bufferDesc);
        }
    }

    // Main constant buffer
    m_constantBuffer = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(SampleConstants), "SampleConstants", caustica::c_MaxRenderPassConstantBufferVersions*2));	// *2 because in some cases we update twice per frame

    // Command list!
    m_commandList = device->createCommandList();

    m_renderCore.initializeRenderPipeline(m_shaderFactory);

    if(device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        m_ommBaker = std::make_shared<OmmBaker>(device, m_DescriptorTable, m_TextureCache, m_shaderFactory);

    // Get all scenes in "assets" folder ??delegated to SceneManager
    m_sceneManager->discoverAvailableScenes(GetLocalPath(c_AssetsFolder));

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
            : FindPreferredScene(m_sceneManager->getAvailableScenes(), preferredScene);
    }

    // Select initial scene
    SetCurrentScene(scene);
}

void PathTracerApp::SetCurrentScene( const std::string & sceneName, bool forceReload )
{
    if (!m_sceneManager->beginSceneSwitch(sceneName, GetLocalPath(c_AssetsFolder), forceReload))
        return;

    m_settings.ResetAccumulation = true;
    SetAsynchronousLoadingEnabled( false );

    m_progressLoading.Stop();
    m_progressLoading.Start("Loading scene...");
    BeginLoadingScene( std::make_shared<caustica::NativeFileSystem>(), m_sceneManager->getCurrentScenePath() );
    if( m_sceneManager->getScene() == nullptr )
    {
        caustica::error( "Unable to load scene '%s'", sceneName.c_str() );
        m_sceneManager->clearScene();
        m_progressLoading.Stop();
        return;
    }
}

bool PathTracerApp::LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return AttachGaussianSplatToScene(fileName, convertRdfToRub);
}

std::filesystem::path
PathTracerApp::ResolveGaussianSplatPath(const GaussianSplat& splat) const
{
    if (splat.path.empty())
        return {};

    std::filesystem::path splatPath = splat.path;
    if (splatPath.is_absolute())
        return splatPath;

    const std::filesystem::path sceneFolder = m_sceneManager->getCurrentScenePath().parent_path();
    if (!sceneFolder.empty() && m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        return sceneFolder / splatPath;

    return std::filesystem::absolute(splatPath);
}

void PathTracerApp::PrepareGaussianSplatPass(GaussianSplatPass& pass)
{
    if (m_renderTargets == nullptr || m_shaderDebug == nullptr)
        return;

    if (m_gpuSort == nullptr)
        m_gpuSort = std::make_shared<GPUSort>(GetDevice(), m_shaderFactory);
    m_gpuSort->CreateRenderPasses(m_CommonPasses, m_shaderDebug);
    pass.SetGpuSort(m_gpuSort);
    pass.CreatePipeline(*m_renderTargets);
}

uint32_t
PathTracerApp::GetTotalGaussianSplatCount() const
{
    uint64_t total = 0;
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.pass != nullptr)
            total += object.pass->GetSplatCount();
    }
    return uint32_t(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

void PathTracerApp::UpdateGaussianSplatUIState()
{
    m_editor.GaussianSplatObjectCount = uint32_t(m_gaussianSplatSceneObjects.size());
    m_editor.GaussianSplatCount = GetTotalGaussianSplatCount();

    m_gaussianSplatFileNameSummary.clear();
    if (m_gaussianSplatSceneObjects.size() == 1 && m_gaussianSplatSceneObjects.front().pass != nullptr)
    {
        m_gaussianSplatFileNameSummary = m_gaussianSplatSceneObjects.front().pass->GetSourceFileName();
    }
    else if (!m_gaussianSplatSceneObjects.empty())
    {
        m_gaussianSplatFileNameSummary = std::to_string(m_gaussianSplatSceneObjects.size()) + " scene Gaussian Splat objects";
    }
    m_editor.GaussianSplatFileName = m_gaussianSplatFileNameSummary;

    if (m_editor.GaussianSplatObjectCount == 0)
        m_editor.SelectedGaussianSplat = false;
}

void PathTracerApp::LoadGaussianSplatsFromScene()
{
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();

    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_shaderFactory)
    {
        UpdateGaussianSplatUIState();
        return;
    }

    SceneGraphWalker walker(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode().get());
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
                caustica::error("Gaussian Splat node '%s' has no path/file field.", walker->GetName().c_str());
            }
            else
            {
                auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
                if (pass->LoadFromFile(splatPath, splat->convertRdfToRub))
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
                    caustica::error("Failed to load Gaussian Splat node '%s' from '%s'.",
                        walker->GetName().c_str(), splatPath.string().c_str());
                }
            }
        }

        walker.Next(true);
    }

    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
}

bool PathTracerApp::AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_sceneManager->getScene()->GetSceneGraph()->GetRootNode())
    {
        caustica::error("Cannot load Gaussian splats before a scene is loaded.");
        return false;
    }
    if (!m_shaderFactory)
    {
        caustica::error("Cannot load Gaussian splats before the shader factory is initialized.");
        return false;
    }

    std::filesystem::path splatPath = fileName;
    if (!splatPath.is_absolute())
        splatPath = std::filesystem::absolute(splatPath);

    if (!std::filesystem::exists(splatPath))
    {
        caustica::error("Gaussian Splat file does not exist: '%s'", splatPath.string().c_str());
        return false;
    }

    auto pass = std::make_unique<GaussianSplatPass>(GetDevice(), m_shaderFactory);
    if (!pass->LoadFromFile(splatPath, convertRdfToRub))
    {
        caustica::error("Failed to load Gaussian Splat file '%s'.", splatPath.string().c_str());
        return false;
    }
    if (pass->GetSplatCount() == 0)
    {
        caustica::error("Gaussian Splat file '%s' contains no splats.", splatPath.string().c_str());
        return false;
    }

    auto splat = std::make_shared<GaussianSplat>();
    splat->path = splatPath.string();
    splat->resolvedPath = splatPath.string();
    splat->convertRdfToRub = convertRdfToRub;
    splat->enabled = true;
    splat->loadedSplatCount = pass->GetSplatCount();

    auto node = std::make_shared<SceneGraphNode>();
    auto sceneGraph = m_sceneManager->getScene()->GetSceneGraph();
    auto rootNode = sceneGraph->GetRootNode();
    node->SetName(MakeUniqueChildNodeName(*rootNode, splatPath.filename().string()));
    node->SetLeaf(splat);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    node->SetTranslation(dm::double3(
        double(m_settings.GaussianSplatTranslation.x),
        double(m_settings.GaussianSplatTranslation.y),
        double(m_settings.GaussianSplatTranslation.z)));
    node->SetRotation(dm::rotationQuat(dm::double3(
        double(m_settings.GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(m_settings.GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(m_settings.GaussianSplatRotationEulerDeg.z) * deg2rad)));
    node->SetScaling(dm::double3(
        double(m_settings.GaussianSplatObjectScale.x),
        double(m_settings.GaussianSplatObjectScale.y),
        double(m_settings.GaussianSplatObjectScale.z)));

    auto attachedNode = sceneGraph->Attach(rootNode, node);
    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    PrepareGaussianSplatPass(*pass);

    GaussianSplatSceneObject object;
    object.splat = splat;
    object.node = attachedNode;
    object.pass = std::move(pass);
    m_gaussianSplatSceneObjects.push_back(std::move(object));

    m_settings.EnableGaussianSplats = true;
    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
    RequestFullRebuild();
    return true;
}

uint32_t
PathTracerApp::GetGaussianSplatCount() const
{
    return GetTotalGaussianSplatCount();
}

uint32_t
PathTracerApp::GetGaussianSplatObjectCount() const
{
    return uint32_t(m_gaussianSplatSceneObjects.size());
}

const std::string&
PathTracerApp::GetGaussianSplatFileName() const
{
    return m_gaussianSplatFileNameSummary;
}

PathTracerApp::GaussianSplatSceneObject*
PathTracerApp::GetPrimaryGaussianSplatObject()
{
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

const PathTracerApp::GaussianSplatSceneObject*
PathTracerApp::GetPrimaryGaussianSplatObject() const
{
    for (const auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

float4x4
PathTracerApp::GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const
{
    auto node = object.node.lock();
    if (node == nullptr)
        return float4x4::identity();

    return dm::affineToHomogeneous(node->GetLocalToWorldTransformFloat());
}

void PathTracerApp::BuildGaussianSplatEmissionProxyList()
{
    m_gaussianSplatEmissionProxies.clear();

    if (!IsGaussianSplatEmissionEnabled(m_ui))
        return;

    const uint32_t maxProxyCount = ClampGaussianSplatEmissionProxyCount(m_settings.GaussianSplatEmissionMaxProxyCount);
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
            m_settings.GaussianSplatScale,
            uint32_t(std::clamp(m_settings.GaussianSplatRtxKernelDegree, 0, 5)),
            m_settings.GaussianSplatRtxAdaptiveClamp,
            m_settings.GaussianSplatTintColor,
            m_settings.GaussianSplatAlphaCullThreshold);

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

void PathTracerApp::SceneUnloading( )
{
    m_editor.TogglableNodes = nullptr;
    SceneRender::SceneUnloading();
    m_bindingSet = nullptr;
    m_renderCore.onSceneUnloading();
    m_bindingCache->Clear( );
    m_lights.clear();
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedNode = nullptr;
    m_editor.SelectedGaussianSplat = false;
    m_gaussianSplatSceneObjects.clear();
    m_gaussianSplatEmissionProxies.clear();
    UpdateGaussianSplatUIState();
    m_gaussianSplatTemporalReset = true;
    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
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

bool PathTracerApp::LoadScene(std::shared_ptr<caustica::IFileSystem> fs, const std::filesystem::path& sceneFileName)
{
    m_progressLoading.Set(10);
    return m_sceneManager->loadScene(std::move(fs), sceneFileName) != nullptr;
}

void PathTracerApp::UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera )
{
    m_renderer->updateCameraFromScene(sceneCamera);
}

void PathTracerApp::UpdateViews( nvrhi::IFramebuffer* framebuffer )
{
    m_renderer->updateViews(framebuffer);
}

void PathTracerApp::CollectUncompressedTextures()
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

void PathTracerApp::RefreshEnvironmentMapMediaList()
{
    const std::filesystem::path currentScenePath = m_sceneManager
        ? m_sceneManager->getCurrentScenePath()
        : std::filesystem::path();

    SceneManager::refreshEnvironmentMapMediaList(
        GetLocalPath(c_AssetsFolder),
        c_EnvMapSubFolder,
        currentScenePath,
        m_envMapMediaList,
        m_envMapMediaFolder);
}

void PathTracerApp::SceneLoaded( )
{
    m_frameIndex = 0;

    RefreshEnvironmentMapMediaList();

    m_progressLoading.Set(50);

    if (m_sampleGame != nullptr) m_sampleGame->SceneLoaded(m_sceneManager->getScene(), m_sceneManager->getCurrentScenePath(), GetLocalPath(c_AssetsFolder));

    m_progressLoading.Set(55);

    SceneRender::SceneLoaded( );

    m_progressLoading.Set(60);

    m_sceneTime = 0.f;
    m_sceneManager->getScene()->FinishedLoading( GetFrameIndex( ) );

    LoadGaussianSplatsFromScene();

    if (!m_initialGaussianSplatAttached && !m_cmdLine.GaussianSplatFileName.empty())
    {
        m_initialGaussianSplatAttached = true;
        (void)AttachGaussianSplatToScene(m_cmdLine.GaussianSplatFileName, m_cmdLine.GaussianSplatConvertRdfToRub);
    }

    m_progressLoading.Set(65);

	// Find lights; do this before special cases to avoid duplicates
	for (auto light : m_sceneManager->getScene()->GetSceneGraph()->GetLights())
	{
		m_lights.push_back(light);
	}

    // seem like sensible defaults
    m_settings.ToneMappingParams.exposureCompensation = 2.0f;
    m_settings.ToneMappingParams.exposureValue = 0.0f;

    std::shared_ptr<EnvironmentLight> envLight = FindEnvironmentLight(m_lights);
    m_envMapLocalPath = (envLight==nullptr)?(""):(envLight->path);
    m_settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;

    if (m_editor.TogglableNodes == nullptr)
    {
        m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*m_editor.TogglableNodes, GetScene()->GetSceneGraph()->GetRootNode().get()); // UNSAFE - make sure not to keep m_editor.TogglableNodes longer than scenegraph!
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
            m_sceneManager->getScene()->GetSceneGraph()->AttachLeafNode(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), envLight);
            m_lights.push_back(envLight);
        }
    }

    auto cameras = m_sceneManager->getScene()->GetSceneGraph( )->GetCameras( );
    auto camScene = (cameras.empty( ))?(nullptr):(std::dynamic_pointer_cast<PerspectiveCamera>(cameras.back()));
    if (camScene)
        UpdateCameraFromScene(camScene);
    else
        m_renderCore.camera().setupDefaultCamera();

    m_renderCore.onSceneLoaded(*m_sceneManager->getScene(), m_editor.AccelerationStructRebuildRequested);

    // PrintSceneGraph( m_sceneManager->getScene()->GetSceneGraph( )->GetRootNode( ) );

    m_editor.ShaderReloadRequested = true;  // we have to re-create shader hit table
    m_settings.EnableAnimations = false;
    m_settings.RealtimeMode = false;

    std::shared_ptr<SampleSettings> settings = m_sceneManager->getScene()->GetSampleSettingsNode();
    if (settings != nullptr)
    {
        m_settings.RealtimeMode = settings->realtimeMode.value_or(m_settings.RealtimeMode);
        m_settings.EnableAnimations = settings->enableAnimations.value_or(m_settings.EnableAnimations);
        if (settings->startingCamera.has_value())
            m_renderCore.camera().setSelectedCameraIndex(settings->startingCamera.value()+1); // slot 0 reserved for free flight camera
        if (settings->realtimeFireflyFilter.has_value())
        {
            m_settings.RealtimeFireflyFilterThreshold = settings->realtimeFireflyFilter.value();
            m_settings.RealtimeFireflyFilterEnabled = true;
        }
        m_settings.BounceCount = settings->maxBounces.value_or(m_settings.BounceCount);
        m_settings.DiffuseBounceCount = settings->maxDiffuseBounces.value_or(m_settings.DiffuseBounceCount);
        m_settings.TexLODBias = settings->textureMIPBias.value_or(m_settings.TexLODBias);
    }

    if (m_cmdLine.stopAnimations)
        m_settings.EnableAnimations = false;

    m_progressLoading.Set(70);

    LocalConfig::PostSceneLoad( static_cast<Sample&>(*this), m_ui );

    m_progressLoading.Set(90);

    if (m_materialsBaker!=nullptr) m_materialsBaker->SceneReloaded();
    if (m_envMapBaker!=nullptr) m_envMapBaker->SceneReloaded();
    if (m_lightsBaker!=nullptr) m_lightsBaker->SceneReloaded();
    if (m_ommBaker!=nullptr) m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    m_progressLoading.Set(100);

    if (m_cmdLine.OverrideToRealtimeMode)
        m_settings.RealtimeMode = true;
    if (m_cmdLine.OverrideToReferenceMode)
        m_settings.RealtimeMode = false;
    
    if (m_cmdLine.OverrideAutoexposureOff)
    {
        m_settings.ToneMappingParams.autoExposure = false;
        m_settings.ToneMappingParams.exposureValue = 0.0f;
    }
    if (m_cmdLine.OverrideExposureOffset != FLT_MAX)
        m_settings.ToneMappingParams.exposureCompensation = m_cmdLine.OverrideExposureOffset;
    if (m_cmdLine.DisableFireflyFilters)
    {
        m_settings.RealtimeFireflyFilterEnabled = false;
        m_settings.ReferenceFireflyFilterEnabled = false;
    }
    if (m_cmdLine.DisablePostProcessFilters)
    {
        m_settings.EnableBloom = false;
    }
    if (m_cmdLine.cameraPosDirUp != "")
        SetCurrentCameraPosDirUp(m_cmdLine.cameraPosDirUp);

    m_settings.MaterialVariantIndex = 0;

    m_asyncLoadingInProgress = true;

#if CAUSTICA_WITH_PYTHON
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

bool PathTracerApp::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_zoomTool && m_zoomTool->KeyboardUpdate(key, scancode, action, mods))
        return true;

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_renderCore.camera().camera().KeyboardUpdate(key, scancode, action, mods);

    if (m_sampleGame && m_sampleGame->KeyboardUpdate(key, scancode, action, mods))
        return true;


    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS && mods != GLFW_MOD_CONTROL && mods != GLFW_MOD_ALT)
    {
        m_settings.EnableAnimations = !m_settings.EnableAnimations;
        return true;
    }
    if( key == GLFW_KEY_F2 && action == GLFW_PRESS )
        m_editor.ShowUI = !m_editor.ShowUI;
    if( key == GLFW_KEY_R && action == GLFW_PRESS && mods == GLFW_MOD_CONTROL )
        m_editor.ShaderReloadRequested = true;

#if CAUSTICA_WITH_STREAMLINE
    if (key == GLFW_KEY_F13 && action == GLFW_PRESS)
    {
        // As GLFW abstracts away from Windows messages
        // We instead set the F13 as the PC_Ping key in the constants and compare against that.
         GetGpuDevice()->GetStreamline().ReflexTriggerPcPing(GetFrameIndex());
    }
#endif

    return true;
}

bool PathTracerApp::MousePosUpdate(double xpos, double ypos)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_renderCore.camera().camera().MousePosUpdate(xpos, ypos);
    if (m_sampleGame)   m_sampleGame->MousePosUpdate(xpos /** upscalingScale.x*/, ypos /** upscalingScale.y*/);

    float2 upscalingScale = float2(1,1);
    if (m_renderTargets != nullptr)
        upscalingScale = float2(m_renderSize)/float2(m_displaySize);

    m_pickPosition = uint2( static_cast<uint>( xpos * upscalingScale.x ), static_cast<uint>( ypos * upscalingScale.y ) );
    m_settings.MousePos = uint2( static_cast<uint>( xpos * upscalingScale.x ), static_cast<uint>( ypos * upscalingScale.y ) );

    if (m_zoomTool)     m_zoomTool->MousePosUpdate( xpos, ypos );

    return true;
}

bool PathTracerApp::MouseButtonUpdate(int button, int action, int mods)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    if (m_zoomTool)
        if (m_zoomTool->MouseButtonUpdate(button, action, mods))
            return true;

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
        m_renderCore.camera().camera().MouseButtonUpdate(button, action, mods);
    if (m_sampleGame)   m_sampleGame->MouseButtonUpdate(button, action, mods);

    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_2)
    {
        m_pick = true;
        m_pickInstance = true;
        m_settings.DebugPixel = m_pickPosition;
    }

#if CAUSTICA_WITH_STREAMLINE
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    {
         GetGpuDevice()->GetStreamline().ReflexTriggerFlash(GetFrameIndex());
    }
#endif

    return true;
}

bool PathTracerApp::MouseScrollUpdate(double xoffset, double yoffset)
{
    // Always forward scroll to ImGui (event-driven, can't poll)
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent((float)xoffset, (float)yoffset);

    if (io.WantCaptureMouse)
        return true; // ImGui consumed it, don't forward to camera

    if (!(m_sampleGame && m_sampleGame->CameraActive()))
    {
        //m_renderCore.camera().camera().MouseScrollUpdate(xoffset, yoffset);
        m_settings.CameraMoveSpeed *= 1.0f + yoffset*0.1f;
    }
    return true;
}

void PathTracerApp::Animate(float fElapsedTimeSeconds)
{
    if (m_settings.ActualFPSLimiter()>0)    // essential for stable video recording
        fElapsedTimeSeconds = 1.0f / (float)m_settings.ActualFPSLimiter();

    m_captureScriptManager->PreAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    // Drain any pending Python scripts. We do this on the renderer thread so
    // bindings observe a coherent scene state and so they can mutate UI
    // settings before rendering for the current frame happens.
    if (m_pythonScripting && IsSceneLoaded())
        m_pythonScripting->ProcessPendingScripts();
#endif

    m_lastDeltaTime = fElapsedTimeSeconds;

    m_renderCore.camera().camera().SetMoveSpeed(m_settings.CameraMoveSpeed);

    if( m_editor.ShaderAndACRefreshDelayedRequest > 0 )
    {
        m_editor.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (m_editor.ShaderAndACRefreshDelayedRequest <= 0 )
        {
            m_editor.ShaderAndACRefreshDelayedRequest = 0;
            m_editor.ShaderReloadRequested = true;
            m_editor.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = m_settings.EnableAnimations && m_settings.RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || m_settings.ResetAccumulation;

    if (m_sampleGame) m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);

    if (m_toneMappingPass)
        m_toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

    if (IsSceneLoaded() && enableAnimationUpdate)
    {
        if (enableAnimations)
            m_sceneTime += fElapsedTimeSeconds;
        if (m_sampleGame && m_sampleGame->IsInitialized())
            m_sceneTime = m_sampleGame->GetGameTime();

        for (const auto& anim : m_sceneManager->getScene()->GetSceneGraph()->GetAnimations())
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

    m_renderCore.camera().selectedCameraIndex() = std::min( m_renderCore.camera().selectedCameraIndex(), GetSceneCameraCount()-1 );
    if (m_renderCore.camera().selectedCameraIndex() > 0)
    {
        std::shared_ptr<caustica::PerspectiveCamera> sceneCamera = std::dynamic_pointer_cast<PerspectiveCamera>(m_sceneManager->getScene()->GetSceneGraph()->GetCameras()[m_renderCore.camera().selectedCameraIndex()-1]);
        if (sceneCamera != nullptr)
            UpdateCameraFromScene( sceneCamera );
    }

    m_renderCore.camera().camera().Animate(fElapsedTimeSeconds);

    if (m_sampleGame) m_sampleGame->TickCamera(fElapsedTimeSeconds, m_renderCore.camera().camera());

    if (m_settings.CameraAntiRRSleepJitter>0)
    {
        float off = 0.05f * ((m_frameIndex%2)?(-m_settings.CameraAntiRRSleepJitter):(m_settings.CameraAntiRRSleepJitter));

        float3 dir = m_renderCore.camera().camera().GetDir();
        float3 right = normalize(cross(dir, m_renderCore.camera().camera().GetUp()));
        affine3 rot = rotation(right, off);
        dir = rot.transformVector(dir);

        m_renderCore.camera().camera().LookTo( m_renderCore.camera().camera().GetPosition(), dir, m_renderCore.camera().camera().GetUp() );
    }

    if (m_renderCore.camera().cameraMovedSinceLastFrame())
    {
        m_renderCore.camera().updateLastCameraState();
        if( !m_settings.RealtimeMode )
            m_settings.ResetAccumulation = true;
        m_gaussianSplatTemporalReset = true;
    }

    m_captureScriptManager->PostAnim();


    double frameTime = GetGpuDevice()->GetAverageFrameTimeSeconds();
    if (frameTime > 0.0)
    {
#if CAUSTICA_WITH_STREAMLINE
        if (m_settings.DLSSFGMultiplier != 1)
            m_fpsInfo = StringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G", frameTime * 1e3, m_settings.DLSSFGMultiplier, m_settings.DLSSFGMultiplier / frameTime);
        else
#endif
            m_fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);
    }

    // Window title
    std::string extraInfo = ", " + m_fpsInfo + ", " + m_sceneManager->getCurrentSceneName() + ", " + GetResolutionInfo() + ", (L: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetLights().size()) + ", MAT: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMaterials().size())
        + ", MESH: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMeshes().size()) + ", I: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetMeshInstances().size()) + ", SI: " + std::to_string(m_sceneManager->getScene()->GetSceneGraph()->GetSkinnedMeshInstances().size())
        //+ ", AvgLum: " + std::to_string((m_renderTargets!=nullptr)?(m_renderTargets->AvgLuminanceLastCaptured):(0.0f))
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";


    GetGpuDevice()->SetInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
}

std::string PathTracerApp::GetResolutionInfo() const
{
    return m_renderer->getResolutionInfo();
}

float PathTracerApp::GetAvgTimePerFrame() const
{
    return m_renderer->getAvgTimePerFrame();
}

std::string PathTracerApp::GetCurrentCameraPosDirUp() const
{
    return m_renderer->getCurrentCameraPosDirUp();
}

bool PathTracerApp::SetCurrentCameraPosDirUp(const std::string & val)
{
    return m_renderer->setCurrentCameraPosDirUp(val);
}

void PathTracerApp::SetCameraVerticalFOV(float cameraFOV)
{
    m_renderer->setCameraVerticalFOV(cameraFOV);
}

void PathTracerApp::SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    m_renderer->setCameraIntrinsics(fx, fy, cx, cy, width, height);
}

void PathTracerApp::ClearCameraIntrinsics()
{
    m_renderer->clearCameraIntrinsics();
}

void PathTracerApp::SaveCurrentCamera() const
{
    m_renderer->saveCurrentCamera();
}

void PathTracerApp::LoadCurrentCamera()
{
    m_renderer->loadCurrentCamera();
}

void PathTracerApp::FillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro> & macros)
{
    macros.clear();

    auto device = GetDevice();
    const bool canUseNvapiHitObject =
        m_settings.NVAPIHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 &&
        device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV);
    const bool canUseDxHitObject =
        m_settings.DXHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;

    assert(!canUseNvapiHitObject || !canUseDxHitObject);

    macros.push_back({ "ENABLE_DEBUG_SURFACE_VIZ",  (m_settings.DebugView != DebugViewType::Disabled)?("1"):("0") });
    macros.push_back({ "ENABLE_DEBUG_LINES_VIZ",    (m_settings.ShowDebugLines)?("1"):("0") });

    macros.push_back({ "USE_NVAPI_HIT_OBJECT_EXTENSION", canUseNvapiHitObject ? "1" : "0" });
    macros.push_back({ "USE_NVAPI_REORDER_THREADS", (canUseNvapiHitObject && m_settings.NVAPIReorderThreads) ? "1" : "0" });

    macros.push_back({ "USE_DX_HIT_OBJECT_EXTENSION", canUseDxHitObject ? "1" : "0" });
    macros.push_back({ "USE_DX_MAYBE_REORDER_THREADS", (canUseDxHitObject && m_settings.DXMaybeReorderThreads) ? "1" : "0" });

    macros.push_back({ "PT_ENABLE_RUSSIAN_ROULETTE", (m_settings.EnableRussianRoulette) ? ("1") : ("0") });

    macros.push_back({ "PT_NEE_ENABLED", (m_settings.UseNEE)?("1"):("0") });

    macros.push_back({ "PT_USE_RESTIR_DI", (m_settings.ActualUseReSTIRDI()) ? ("1") : ("0") });   // these will match constants.useReSTIRDI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_GI", (m_settings.ActualUseReSTIRGI()) ? ("1") : ("0") });   // these will match constants.useReSTIRGI but constants are used in other passes too
    macros.push_back({ "PT_USE_RESTIR_PT", (m_settings.ActualUseReSTIRPT()) ? ("1") : ("0") });
    

    // minor perf gains but recompile time every time value changed is too annoying 
    // macros.push_back({ "PT_BOUNCE_COUNT", std::to_string(m_settings.BounceCount) });
    // macros.push_back({ "PT_DIFFUSE_BOUNCE_COUNT", std::to_string((m_settings.RealtimeMode) ? (m_settings.RealtimeDiffuseBounceCount) : (m_settings.ReferenceDiffuseBounceCount)) });

    macros.push_back({ "CAUSTICA_USE_APPROXIMATE_MIS", (m_settings.ActualUseApproximateMIS()) ? ("1") : ("0") });

    // It helps performance a lot when these are baked in
    macros.push_back({ "CAUSTICA_NEE_FULL_SAMPLE_COUNT", std::to_string(m_settings.NEEFullSamples) });
    uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
    uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_settings.ActualNEEAT_LocalToGlobalSampleRatio(), m_settings.NEECandidateSamples);
    macros.push_back({ "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(m_settings.NEECandidateSamples) });

    macros.push_back({ "CAUSTICA_DISABLE_SER_TERMINATION_HINT", (m_settings.DbgDisableSERTerminationHint)?("1"):("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NON_NEE_LIGHTING", (m_settings.DbgDiscardNonNEELighting) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NEE_LIGHTING", (m_settings.DbgDiscardNEELighting) ? ("1") : ("0") });
    
    macros.push_back({ "CAUSTICA_FIREFLY_FILTER", (m_settings.ActualFireflyFilterEnabled()) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", std::to_string(m_settings.StablePlanesActiveCount) });

    macros.push_back({ "CAUSTICA_NESTED_DIELECTRICS_QUALITY", std::to_string(m_settings.NestedDielectricsQuality) });

    macros.push_back({ "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", (m_settings.UseFp16Types) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", (m_settings.EnableLDSamplerForBSDF) ? ("1") : ("0") });

    m_lightsBaker->SetGlobalShaderMacros(macros);
    if (m_ommBaker != nullptr)
        m_ommBaker->SetGlobalShaderMacros(macros);
}

extern HitGroupInfo ComputeSubInstanceHitGroupInfo(const PTMaterial& material);

bool PathTracerApp::CreatePTPipeline(caustica::ShaderFactory& shaderFactory)
{
    {
        std::vector<caustica::ShaderMacro> shaderMacros;
		// shaderMacros.push_back(caustica::ShaderMacro({ "USE_RTXDI", "0" }));
        m_exportVBufferCS = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
		pipelineDesc.CS = m_exportVBufferCS;
        m_exportVBufferPSO = GetDevice()->createComputePipeline(pipelineDesc);
    }

    return true;
}

void PathTracerApp::CreateBlases(nvrhi::ICommandList* commandList)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings.AS.ExcludeTransmissive };
    m_renderCore.accelStructs().createBlases(commandList, *m_sceneManager->getScene(), settings);
}

void PathTracerApp::UploadSubInstanceData(nvrhi::ICommandList* commandList)
{
    m_renderCore.accelStructs().uploadSubInstanceData(commandList);
}

void PathTracerApp::CreateTlas(nvrhi::ICommandList* commandList)
{
    m_renderCore.accelStructs().createTlas(commandList, *m_sceneManager->getScene());
}

void PathTracerApp::CreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if(m_ommBaker) m_ommBaker->CreateOpacityMicromaps(*m_sceneManager->getScene());
    CreateBlases(commandList);
    CreateTlas(commandList);
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        if (ResolveGaussianSplatShadowMode(m_ui) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            object.pass->BuildAccelerationStructures(
                commandList,
                m_settings.GaussianSplatUseAABBs,
                m_settings.GaussianSplatUseTLASInstances,
                m_settings.GaussianSplatBlasCompaction,
                m_settings.GaussianSplatScale,
                uint32_t(std::clamp(m_settings.GaussianSplatRtxKernelDegree, 0, 5)),
                m_settings.GaussianSplatRtxAdaptiveClamp);
        else
            object.pass->ReleaseAccelerationStructures();
    }
}

void PathTracerApp::RecreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if (m_editor.AccelerationStructRebuildRequested)
    {
        m_editor.AccelerationStructRebuildRequested = false;
        m_settings.ResetAccumulation = true;

        GetDevice()->waitForIdle();

        m_bindingSet = nullptr;
        m_renderCore.accelStructs().releaseGpuResources();

        m_renderCore.accelStructs().clearMeshAccelStructs(*m_sceneManager->getScene());
        GetDevice()->runGarbageCollection();

        commandList->open();
        CreateAccelStructs(commandList);
        commandList->close();
        GetDevice()->executeCommandList(commandList);
        GetDevice()->waitForIdle();
    }
}

void PathTracerApp::RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    m_settings.ResetAccumulation = true;

    if (!m_renderCore.accelStructs().hasTopLevelAS())
    {
        m_editor.AccelerationStructRebuildRequested = true;
        return;
    }

    m_renderCore.accelStructs().requestMeshRebuild(mesh);
}

void PathTracerApp::RebuildDirtyMeshAccelStructs(nvrhi::ICommandList* commandList)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings.AS.ExcludeTransmissive };
    m_renderCore.accelStructs().rebuildDirtyMeshes(commandList, *m_sceneManager->getScene(), settings, m_editor.AccelerationStructRebuildRequested);
}

void PathTracerApp::TransitionMeshBuffersToReadOnly(nvrhi::ICommandList* commandList)
{
    m_commandList = commandList;
    m_renderer->transitionMeshBuffersToReadOnly();
}

void PathTracerApp::UpdateSkinnedBLASs(nvrhi::ICommandList* commandList, uint32_t frameIndex) const
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings.AS.ExcludeTransmissive };
    m_renderCore.accelStructs().updateSkinnedBlases(commandList, *m_sceneManager->getScene(), settings, frameIndex);
}

void PathTracerApp::BuildTLAS(nvrhi::ICommandList* commandList) const
{
    caustica::AccelStructBuildSettings settings = {
        .excludeTransmissive = m_settings.AS.ExcludeTransmissive,
        .forceOpaque         = m_settings.AS.ForceOpaque,
    };
    caustica::OmmAccelStructState ommState = {};
    if (m_ommBaker)
    {
        const auto& ommUI = m_ommBaker->UIData();
        ommState.enabled = ommUI.Enable;
        ommState.force2State = ommUI.Force2State;
        ommState.onlyOMMs = ommUI.OnlyOMMs;
        ommState.debugViewEnabled = ommUI.DebugView != OpacityMicroMapDebugView::Disabled;
    }
    m_renderCore.accelStructs().buildTlas(commandList, *m_sceneManager->getScene(), settings, ommState, m_ommBaker.get());
}


void PathTracerApp::BackBufferResizing()
{
    SceneRender::BackBufferResizing();
    
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
#if CAUSTICA_WITH_STREAMLINE
    if (!GetGpuDevice()->GetDeviceParams().headlessDevice &&
        (m_settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_settings.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        GetGpuDevice()->GetStreamline().CleanupDLSS(false);
        GetGpuDevice()->GetStreamline().CleanupDLSSG(false);

        if (GetGpuDevice()->GetStreamline().IsDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            GetGpuDevice()->GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            GetGpuDevice()->GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_settings.DLSSFGOptions = dlssgOptions;
        }
    }
#endif
}

void PathTracerApp::CreateRenderPasses( bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList )
{
    m_bindingCache->Clear();

    const uint2 screenResolution = {m_renderTargets->OutputColor->getDesc().width, m_renderTargets->OutputColor->getDesc().height};

    m_shaderDebug = std::make_shared<ShaderDebug>(GetDevice(), initializeCommandList, m_shaderFactory, m_CommonPasses);

    if (m_settings.ActualUseRTXDIPasses())
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
    m_toneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->LdrFramebuffer, *m_renderCore.camera().view(), m_renderTargets->OutputColor);
    m_bloomPass = std::make_unique<BloomPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->ProcessedOutputFramebuffer, *m_renderCore.camera().view());
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

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, *m_renderCore.camera().view(), taaParams);
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

void PathTracerApp::SetEnvMapOverrideSource(const std::string& envMapOverride) 
{ 
    if (m_envMapOverride != envMapOverride && m_envMapBaker != nullptr)
        m_envMapBaker->SetTargetCubeResolution(0);  // reset resolution just to avoid getting crazy with procedural sky as it's very slow
    m_envMapOverride = envMapOverride; 
}


void PathTracerApp::PreUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    RAII_SCOPE(m_commandList->beginMarker("PreUpdateLighting"); , m_commandList->endMarker(); );

    auto preUpdateCube = m_envMapBaker->GetEnvMapCube();

    std::filesystem::path sceneDirectory;
    if (m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_sceneManager->getCurrentScenePath().parent_path();

    std::string envMapActualPath = m_envMapLocalPath; 
    if (m_envMapOverride != "" && m_envMapOverride != c_EnvMapSceneDefault)
        envMapActualPath = (IsProceduralSky(m_envMapOverride.c_str()))?(m_envMapOverride):(std::string(c_EnvMapSubFolder) + "/" + m_envMapOverride);

    if (!envMapActualPath.empty() && !IsProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = ResolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    m_envMapBaker->PreUpdate(commandList, m_CommonPasses, envMapActualPath, sceneDirectory);

    if (preUpdateCube != m_envMapBaker->GetEnvMapCube())
        needNewBindings = true;
}

void PathTracerApp::UpdateLighting(nvrhi::CommandListHandle commandList)
{
    RAII_SCOPE( m_commandList->beginMarker("UpdateLighting");, m_commandList->endMarker(); );

    EMB_DirectionalLight dirLights[EnvMapBaker::c_MaxDirLights];
    uint dirLightCount = 0;
    {   // Find and pre-process directional analytic lights, and convert them to environment map local frame so they remain pointing in correct world direction!
        float3 rotationInRadians = radians(m_settings.EnvironmentMapParams.RotationXYZ);
        affine3 rotationTransform = dm::rotation(rotationInRadians);
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

    if (m_envMapBaker->Update(commandList, *m_bindingCache, m_CommonPasses, EnvMapBaker::BakeSettings { .EnvMapRadianceScale = c_envMapRadianceScale }, m_sceneTime, dirLights, dirLightCount, !m_settings.RealtimeMode || !m_settings.EnableAnimations) )
        m_settings.ResetAccumulation = true;

    {
        LightsBaker::BakeSettings settings;
        settings.ImportanceSamplingType = (uint)m_settings.NEEType;
        settings.CameraPosition = m_renderCore.camera().camera().GetPosition();
        settings.CameraDirection = m_renderCore.camera().camera().GetDir();
        settings.ViewProjMatrix = m_renderCore.camera().view()->GetViewProjectionMatrix();
        settings.MouseCursorPos = m_settings.MousePos;
        settings.GlobalTemporalFeedbackWeight   = m_settings.NEEAT_GlobalTemporalFeedbackWeight;
        settings.LocalToGlobalSampleRatio       = m_settings.ActualNEEAT_LocalToGlobalSampleRatio();
        settings.UseApproximateMIS              = m_settings.ActualUseApproximateMIS();
        settings.DistantVsLocalImportanceScale  = m_settings.NEEAT_Distant_vs_Local_Importance;
        settings.ResetFeedback = m_settings.ResetAccumulation && !m_settings.RealtimeMode 
#if 1
            || m_settings.ResetRealtimeCaches
#endif
        ;
        settings.PrevViewportSize = float2( (float)m_renderCore.camera().viewPrevious()->GetViewExtent().width(), (float)m_renderCore.camera().viewPrevious()->GetViewExtent().height() );
        settings.ViewportSize = float2( (float)m_renderCore.camera().view()->GetViewExtent().width(), (float)m_renderCore.camera().view()->GetViewExtent().height() );
        settings.EnvMapParams = LightsBakerEnvMapParams{ .Transform = m_envMapSceneParams.Transform, .InvTransform = m_envMapSceneParams.InvTransform, .ColorMultiplier = m_envMapSceneParams.ColorMultiplier, .Enabled = m_envMapSceneParams.Enabled };
        settings.FrameIndex = m_frameIndex;

        BuildGaussianSplatEmissionProxyList();
        if (!m_gaussianSplatEmissionProxies.empty() && IsGaussianSplatEmissionEnabled(m_ui))
        {
            settings.GaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
            settings.GaussianSplatEmissionObjectToWorld = float4x4::identity();
            settings.GaussianSplatEmissionIntensity = m_settings.GaussianSplatEmissionIntensity;
        }

        m_lightsBaker->UpdateBegin(commandList, *m_bindingCache, settings, m_sceneTime, m_sceneManager->getScene(), m_materialsBaker, m_ommBaker, m_renderCore.accelStructs().getSubInstanceBuffer(), m_renderCore.accelStructs().getSubInstanceData(), m_envMapBaker->GetImportanceSampling()->GetRadianceAndImportanceMap());
    }
}

void PathTracerApp::PreUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_settings.RealtimeMode && (resetAccum || m_settings.ResetAccumulation || m_settings.ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_settings.ReferenceOIDNDenoiserChanged)
    {
        ResetReferenceOIDN();
        m_settings.ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_settings.ResetAccumulation;
    resetAccum |= m_settings.RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_settings.AccumulationPreWarmRealtimeCaches)?(-32):(0);
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
    } else if( m_accumulationSampleIndex < m_settings.AccumulationTarget )
    {
        m_benchFrames++;
        m_benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_settings.RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_settings.AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_settings.AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_settings.DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}

void PathTracerApp::PostUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_settings.AccumulationTarget );

    if (m_settings.ActualUseRTXDIPasses())
        m_rtxdiPass->EndFrame();

    m_settings.ResetAccumulation = false;
    m_settings.ResetRealtimeCaches = false;
    m_frameIndex++;
}

void PathTracerApp::UpdatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData )
{
#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
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
#endif // CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE

    constants.camera = cameraData;
    constants.prevCamera = cameraData;
    if (m_renderCore.camera().viewPrevious())
        constants.prevCamera.PosW = m_renderCore.camera().viewPrevious()->GetInverseViewMatrix().m_translation;

    constants.bounceCount = m_settings.BounceCount;
    constants.diffuseBounceCount = m_settings.DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_settings.RealtimeMode == false && m_settings.AccumulationAA)?(1):( (m_settings.RealtimeMode && m_settings.RealtimeAA == 3)?(m_settings.DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -dm::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_settings.TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_settings.ActualSamplesPerPixel();

    //constants.subSampleCount = m_settings.ActualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_settings.ActualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x; assert( m_renderSize.x == m_renderTargets->OutputColor->getDesc().width );
    constants.imageHeight = m_renderSize.y; assert( m_renderSize.y == m_renderTargets->OutputColor->getDesc().height );

    // this is the dynamic luminance that when passed through current tonemapper with current exposure settings, produces the same 50% gray
    constants.preExposedGrayLuminance = m_settings.EnableToneMapping?(dm::luminance(m_toneMappingPass->GetPreExposedGray(0))):(1.0f);

    const float disabledFF = 0.0f;
    if (m_settings.RealtimeMode)
        constants.fireflyFilterThreshold = (m_settings.RealtimeFireflyFilterEnabled)?(m_settings.RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_settings.ReferenceFireflyFilterEnabled)?(m_settings.ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_settings.ActualUseReSTIRDI();
    constants.useReSTIRGI = m_settings.ActualUseReSTIRGI();
    constants.useReSTIRPT = m_settings.ActualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_settings.EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_settings.DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_settings.DLSSRRBrightnessClampK>0)?(m_settings.DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_settings.ActualUseStandaloneDenoiser() || m_settings.RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_settings.StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_settings.StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_settings.BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_settings.AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_settings.StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_settings.StablePlanesSuppressPrimaryIndirectSpecular?m_settings.StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_settings.StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //GetFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_settings.UseNEE;
    constants.NEEType                           = m_settings.NEEType;
    constants.NEECandidateSamples               = m_settings.NEECandidateSamples;
    constants.NEEFullSamples                    = m_settings.NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_settings.EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_settings.STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_settings.STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_settings.STFFilterMode);
    constants.STFGaussianSigma                  = m_settings.STFGaussianSigma;
#endif
}


void PathTracerApp::RtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_settings.EnvironmentMapParams.Enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_renderCore.camera().camera().GetPosition();
	bridgeParameters.userSettings = m_settings.RTXDI;
    bridgeParameters.usingLightSampling = m_settings.ActualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_settings.ActualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    BuildGaussianSplatEmissionProxyList();
    if (!m_gaussianSplatEmissionProxies.empty() && IsGaussianSplatEmissionEnabled(m_ui))
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_settings.GaussianSplatEmissionIntensity;
    }

    if( m_settings.ResetRealtimeCaches )
        m_rtxdiPass->Reset();

	m_rtxdiPass->PrepareResources(m_commandList, *m_renderTargets, envMapPresent ? m_envMapBaker : nullptr, m_envMapSceneParams,
        m_sceneManager->getScene(), m_materialsBaker, m_ommBaker, m_renderCore.accelStructs().getSubInstanceBuffer(), bridgeParameters, m_bindingLayout, m_shaderDebug );
 }

bool PathTracerApp::ShouldRenderUnfocused()
{
    if (m_frameIndex < 16 || m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_captureScriptManager->IsDoingWork() )
    {
        // Make sure we at least run one render frame to allow expensive resource creation to happen in background, and to allow at least somewhat decent convergence so when user alt-tabs they get a nice image
        return true;
    }

    if (m_editor.RenderWhenOutOfFocus)
    {
        return true;
    }

    // Let Reference mode accumulate all frames before pausing
    return (!m_settings.RealtimeMode && (m_accumulationSampleIndex < m_settings.AccumulationTarget));
}

void PathTracerApp::StreamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (GetGpuDevice()->GetDeviceParams().headlessDevice)
        return;

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_settings.ActualReflexMode();
        reflexConsts.frameLimitUs = m_settings.ReflexCappedFps == 0 ? 0 : int(1000000. / m_settings.ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        GetGpuDevice()->GetStreamline().SetReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        GetGpuDevice()->GetStreamline().GetReflexState(reflexState);
        if (m_settings.IsReflexSupported)
        {
            m_settings.IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_settings.IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

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

                m_settings.ReflexStats = "frameID: " + std::to_string(frameID);
                m_settings.ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_settings.ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_settings.ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_settings.ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_settings.ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_settings.ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_settings.ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_settings.ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (m_settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn && m_settings.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOff) {
            GetGpuDevice()->GetStreamline().CleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = m_settings.ActualDLSSFGMode();
        dlssgOptions.numFramesToGenerate = m_settings.DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (GetGpuDevice()->GetStreamline().IsDLSSGAvailable())
        {
            StreamlineInterface::DLSSGState state;
            GetGpuDevice()->GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            GetGpuDevice()->GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_settings.DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_settings.RealtimeAA == 3 && !m_settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_settings.RealtimeAA = 2;
    }
    if ( m_settings.RealtimeAA == 2 && !m_settings.IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_settings.RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_settings.RealtimeAA >= 2 && m_settings.RealtimeAA <= 3) && m_settings.DLSSLastRealtimeAA != m_settings.RealtimeAA;
    {
        // Reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_settings.DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_settings.DLSSLastMode = SampleUIData::DLSSModeDefault;
            m_settings.DLSSMode = SampleUIData::DLSSModeDefault;
            m_settings.DLSSLastDisplaySize = { 0,0 };
        }

        m_settings.DLSSLastRealtimeAA = m_settings.RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3) && m_settings.RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_settings.IsDLSSSuported)
            {
                dlssOptions.mode = m_settings.DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_settings.RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    GetGpuDevice()->GetStreamline().SetDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_settings.RealtimeAA = 1" - check for recent code changes.
            }

            if (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_settings.DLSSMode != m_settings.DLSSLastMode) || (m_displaySize.x != m_settings.DLSSLastDisplaySize.x) || (m_displaySize.y != m_settings.DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    GetGpuDevice()->GetStreamline().QueryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_settings.RealtimeAA = 0;
                        m_settings.DLSSMode = SampleUIData::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_settings.DLSSLastMode = m_settings.DLSSMode;
                        m_settings.DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_settings.RealtimeAA == 3) // DLSS-RR
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
                dlssRROptions.preset                = m_settings.DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_settings.IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                GetGpuDevice()->GetStreamline().SetDLSSOptions(dlssOptions);
            }

            m_renderSize = m_displaySize;
        }
    }
#else
    const bool changeToDLSSMode = false;
#endif // #if CAUSTICA_WITH_STREAMLINE
}

#if CAUSTICA_WITH_NATIVE_DLSS
void PathTracerApp::NativeDLSSPreRender()
{
    if (!m_settings.RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
    }

    if (m_settings.RealtimeAA == 3 && !m_settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_settings.RealtimeAA = 2;
    }

    if (m_settings.RealtimeAA == 2 && !m_settings.IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_settings.RealtimeAA = 1;
    }

    const bool usingDLSS = (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_settings.DLSSLastRealtimeAA != m_settings.RealtimeAA;

    if (changeToDLSSMode || m_settings.DLSSMode == SI::DLSSMode::eOff)
    {
        m_settings.DLSSLastMode = SampleUIData::DLSSModeDefault;
        m_settings.DLSSMode = SampleUIData::DLSSModeDefault;
        m_settings.DLSSLastDisplaySize = { 0, 0 };
    }

    m_settings.DLSSLastRealtimeAA = m_settings.RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_settings.DLSSMode != m_settings.DLSSLastMode) ||
            (m_displaySize.x != m_settings.DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_settings.DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_settings.DLSSLastMode = m_settings.DLSSMode;
            m_settings.DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_settings.DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif

void PathTracerApp::SetSceneTime( double sceneTime ) 
{ 
    if (m_sampleGame && m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_sceneTime = sceneTime; 
}

double PathTracerApp::GetSceneTime()
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        return m_sampleGame->GetGameTime();
    return m_sceneTime;
}


void PathTracerApp::PreRender()
{
    // Limit FPS
    if (m_settings.ActualFPSLimiter() > 0)
        g_FPSLimiter.FramerateLimit(m_settings.ActualFPSLimiter());

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

void PathTracerApp::PostProcessPreToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{ // a.k.a. HDR post-process (e.g. bloom goes here)
    caustica::PlanarView fullscreenView = *m_renderCore.camera().view();
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    if (m_settings.EnableBloom && m_settings.BloomIntensity > 0.f && m_settings.BloomRadius > 0.f)
    {
        m_bloomPass->Render(m_commandList, m_renderTargets->ProcessedOutputFramebuffer, fullscreenView, m_renderTargets->ProcessedOutputColor, m_settings.BloomRadius, m_settings.BloomIntensity);
    }

    if (m_settings.PostProcessTestPassHDR)
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

void PathTracerApp::PostProcessPostToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{ // a.k.a. LDR post-process (e.g. colour filters go here)
    if (m_settings.PostProcessEdgeDetection)
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

        SampleMiniConstants miniConstants = { uint4( *reinterpret_cast<uint*>(&m_settings.PostProcessEdgeDetectionThreshold), 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        m_commandList->dispatchRays(args);

        m_commandList->setTextureState(m_renderTargets->LdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_commandList->endMarker();
    }
}

void PathTracerApp::RenderGaussianSplats(bool renderToOutputColor)
{
    if (!m_settings.EnableGaussianSplats || m_gaussianSplatSceneObjects.empty())
        return;

    const bool stochasticSplats = m_settings.EnableGaussianSplats && m_settings.GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_ui);
    GaussianSplatRenderSettings settings;
    settings.enabled = m_settings.EnableGaussianSplats;
    settings.depthTest = m_settings.GaussianSplatDepthTest;
    settings.sortingMode = m_settings.GaussianSplatSortingMode == 1 ? GaussianSplatSortMode::StochasticSplats : GaussianSplatSortMode::GpuSort;
    settings.renderTarget = renderToOutputColor ? GaussianSplatRenderTarget::OutputColor : GaussianSplatRenderTarget::ProcessedOutputColor;
    settings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(dm::clamp(m_settings.GaussianSplatFrustumCulling, 0, 2));
    settings.projectionMethod = GaussianSplatProjectionMethod::Eigen;
    settings.shFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_settings.GaussianSplatSHFormat, 0, 2));
    settings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_settings.GaussianSplatRGBAFormat, 0, 2));
    settings.screenSizeCulling = m_settings.GaussianSplatScreenSizeCulling;
    settings.mipSplattingAntialiasing = m_settings.GaussianSplatMipAntialiasing;
    settings.useAABBs = m_settings.GaussianSplatUseAABBs;
    settings.useTLASInstances = m_settings.GaussianSplatUseTLASInstances;
    settings.blasCompaction = m_settings.GaussianSplatBlasCompaction;
    settings.splatScale = m_settings.GaussianSplatScale;
    settings.alphaScale = m_settings.GaussianSplatAlphaScale;
    settings.brightness = m_settings.GaussianSplatBrightness;
    settings.tintColor = m_settings.GaussianSplatTintColor;
    settings.alphaCullThreshold = m_settings.GaussianSplatAlphaCullThreshold;
    settings.shadowsEnabled = gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    settings.shadowMode = gaussianSplatShadowMode;
    settings.shadowStrength = m_settings.GaussianSplatShadowStrength;
    settings.shadowRayOffset = m_settings.GaussianSplatRtxParticleShadowOffset;
    settings.shadowSoftRadius = m_settings.GaussianSplatShadowSoftRadius;
    settings.shadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_settings.GaussianSplatShadowSoftSampleCount);
    settings.shadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
    settings.frustumDilation = m_settings.GaussianSplatFrustumDilation;
    settings.minPixelCoverage = m_settings.GaussianSplatMinPixelCoverage;
    if (stochasticSplats && m_settings.RealtimeMode)
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

    caustica::PlanarView splatView = *m_renderCore.camera().view();
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
        object.pass->Render(m_commandList, splatView, m_renderCore.accelStructs().getTopLevelAS().Get(), *m_renderTargets, settings);
        renderedAny = true;
    }

    if (renderedAny && stochasticSplats && !renderToOutputColor)
        AccumulateGaussianSplats(splatView);
}

void PathTracerApp::AccumulateGaussianSplats(const caustica::IView& splatView)
{
    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_settings.ResetAccumulation || m_settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset)
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

void PathTracerApp::Render(nvrhi::IFramebuffer* framebuffer)
{
    const auto& fbinfo = framebuffer->getFramebufferInfo();
    m_displaySize = m_renderSize = uint2(fbinfo.width, fbinfo.height);
    float lodBias = 0.f;

    if (m_sceneManager->getScene() == nullptr)
    {
        assert( false ); // TODO: handle separately, just display pink color
        return;
    }
    m_progressLoading.Stop();
    m_asyncLoadingInProgress = false; // can be changed back to 'true' during the frame below, for example by OMMs

    HandleDroppedFiles();

    PreRender();

    StreamlinePreRender();
#if CAUSTICA_WITH_NATIVE_DLSS
    NativeDLSSPreRender();
#endif

 
    m_displayAspectRatio = m_displaySize.x/(float)m_displaySize.y;

    m_renderCore.camera().ensureViews(m_renderSize);

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
    if (m_settings.EnvironmentMapParams.Enabled)
    {
        float intensity = m_settings.EnvironmentMapParams.Intensity / c_envMapRadianceScale;
        m_envMapSceneParams.ColorMultiplier = m_settings.EnvironmentMapParams.TintColor * intensity;

        float3 rotationInRadians = dm::radians(m_settings.EnvironmentMapParams.RotationXYZ);
        affine3 rotationTransform = dm::rotation(rotationInRadians);
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

    if (m_editor.ShaderReloadRequested)
    {
        m_editor.ShaderReloadRequested = false;
        m_shaderFactory->ClearCache();
        needNewPasses = true;
    }

    bool exposureResetRequired = false;

    if (m_settings.NRDModeChanged) // if changing between ReLAX and ReBLUR
    {
        needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_settings.ActualUseStandaloneDenoiser()) // clean up the memory if not used
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
            
            std::vector<std::filesystem::path> additionalShaderPaths;
            m_computePipelineBaker = std::make_shared<ComputePipelineBaker>(GetDevice(), additionalShaderPaths);
            
            CreateRTPipelines();
        }

        m_materialsBaker->CreateRenderPassesAndLoadMaterials(m_bindlessLayout, m_CommonPasses, m_sceneManager->getScene(), m_sceneManager->getCurrentScenePath(), GetLocalPath(c_AssetsFolder));
        m_progressInitializingRenderer.Set(5);
        CollectUncompressedTextures();
        if(m_ommBaker) m_ommBaker->CreateRenderPasses(m_bindlessLayout, m_CommonPasses);
        m_progressInitializingRenderer.Set(20);

        if (m_zoomTool == nullptr)
            m_zoomTool = std::make_unique<ZoomTool>(GetDevice(), m_shaderFactory);
    }

    // Changes to material properties and settings can require a BLAS/TLAS or subInstanceBuffer rebuild (alpha tested/exclusion flags etc); otherwise this is a no-op.
    RecreateAccelStructs(m_commandList);

    if (m_settings.ActualUseRTXDIPasses() && m_rtxdiPass == nullptr )
        needNewPasses = true; // this will initialize rtxdi passes
    if (!m_settings.ActualUseRTXDIPasses())
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
    m_ptPipelineBaker->Update(m_sceneManager->getScene(), (unsigned int)m_renderCore.accelStructs().getSubInstanceData().size(), [this](std::vector<caustica::ShaderMacro> & macros){ this->FillPTPipelineGlobalMacros(macros); }, needNewPasses);
    
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
            nvrhi::Viewport viewport = m_renderCore.camera().view()->GetViewport();
            float2 jitter = m_renderCore.camera().view()->GetPixelOffset();
            float4x4 projMatrix = m_renderCore.camera().view()->GetProjectionMatrix();
            float2 viewSize = { viewport.maxX - viewport.minX, viewport.maxY - viewport.minY };
            float outputAspectRatio = m_displayAspectRatio; //windowViewport.width() / windowViewport.height();    // render and display outputs might not match in case of lower DLSS/etc resolution rounding!
            bool rowMajor = true;
            float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
            float fovY = atanf(tanHalfFOVY) * 2.0f;
            cameraData = BridgeCamera(uint(viewSize.x), uint(viewSize.y), outputAspectRatio, m_renderCore.camera().camera().GetPosition(), m_renderCore.camera().camera().GetDir(), m_renderCore.camera().camera().GetUp(), fovY, m_renderCore.camera().zNear(), 1e7f, m_settings.CameraFocalDistance, m_settings.CameraAperture, jitter);
        }

        if (needNewPasses || needNewBindings || m_bindingSet == nullptr)
            m_shaderDebug->CreateRenderPasses(framebuffer, m_renderTargets->Depth);

        if (m_settings.EnableShaderDebug)
        {
            dm::float4x4 viewProj = m_renderCore.camera().view()->GetViewProjectionMatrix();
            m_shaderDebug->BeginFrame(m_commandList, viewProj);
        }

        // NOTE: this refreshes geometry buffers and updates stuff needed by m_ommBaker and m_materialsBaker and others below!
        m_sceneManager->getScene()->Refresh(m_commandList, GetFrameIndex());

        if(m_ommBaker) m_ommBaker->BuildOpacityMicromaps(*m_commandList, *m_sceneManager->getScene());
        RebuildDirtyMeshAccelStructs(m_commandList);
        UpdateSkinnedBLASs(m_commandList, GetFrameIndex());
        m_commandList->compactBottomLevelAccelStructs(); // Compact acceleration structures that are tagged for compaction and have finished executing the original build
        BuildTLAS(m_commandList);
        TransitionMeshBuffersToReadOnly(m_commandList);
        if (m_ommBaker) 
        {
            m_asyncLoadingInProgress |= m_ommBaker->Update(*m_commandList, *m_sceneManager->getScene());
            m_asyncLoadingInProgress |= m_ommBaker->UIData().BuildsLeftInQueue > 0;
        }

        m_materialsBaker->Update(m_commandList, m_sceneManager->getScene(), m_renderCore.accelStructs().getSubInstanceData());
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

    m_toneMappingPass->PreRender(m_settings.ToneMappingParams);

    PreUpdatePathTracing(needNewPasses, m_commandList);

    // I suppose we need to clear depth for right-click picking at least
    m_renderTargets->Clear( m_commandList );

    SampleConstants & constants = m_currentConstants; memset(&constants, 0, sizeof(constants));
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) }; // accessible but unused in path tracing at the moment
    if( m_sceneManager->getScene() == nullptr )
    {
        m_commandList->clearTextureFloat( m_renderTargets->OutputColor, nvrhi::AllSubresources, nvrhi::Color( 1, 1, 0, 0 ) );
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }
    else
    {
        UpdatePathTracerConstants(constants.ptConsts, cameraData);
        constants.MaterialCount = m_materialsBaker->GetMaterialDataCount(); // m_sceneManager->getScene()->GetSceneGraph()->GetMaterials().size();
        const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_ui);
        const GaussianSplatSceneObject* primaryGaussianSplat = GetPrimaryGaussianSplatObject();
        GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianSplat != nullptr ? primaryGaussianSplat->pass.get() : nullptr;
        constants.GaussianSplatShadowCount = (m_settings.EnableGaussianSplats
                && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
                && primaryGaussianSplatPass != nullptr
                && primaryGaussianSplatPass->GetTopLevelAS() != nullptr)
            ? primaryGaussianSplatPass->GetSplatCount()
            : 0;
        constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0 ? 1u : 0u;
        constants.GaussianSplatShadowScale = m_settings.GaussianSplatScale;
        constants.GaussianSplatShadowAlphaThreshold = m_settings.GaussianSplatAlphaCullThreshold;
        constants.GaussianSplatShadowUseTLASInstances =
            (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetShadowUsesTLASInstances()) ? 1u : 0u;
        constants.GaussianSplatShadowPrimitiveCountPerSplat =
            primaryGaussianSplatPass != nullptr ? primaryGaussianSplatPass->GetShadowPrimitiveCountPerSplat() : 1u;
        constants.GaussianSplatShadowMode = constants.GaussianSplatShadowsEnabled != 0
            ? gaussianSplatShadowMode
            : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        constants.GaussianSplatShadowSoftRadius = m_settings.GaussianSplatShadowSoftRadius;
        constants.GaussianSplatShadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_settings.GaussianSplatShadowSoftSampleCount);
        constants.GaussianSplatShadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
        constants.GaussianSplatShadowRayOffset = m_settings.GaussianSplatRtxParticleShadowOffset;
        constants.GaussianSplatShadowAlphaScale = m_settings.GaussianSplatAlphaScale;
        constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatKernelMinResponse;
        constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(m_settings.GaussianSplatRtxKernelDegree, 0, 5));
        constants.GaussianSplatShadowAdaptiveClamp = m_settings.GaussianSplatRtxAdaptiveClamp ? 1u : 0u;
        constants.GaussianSplatShadowWorldToObject = primaryGaussianSplat != nullptr
            ? inverse(GetGaussianSplatObjectToWorld(*primaryGaussianSplat))
            : float4x4::identity();

        constants.envMapSceneParams = m_envMapSceneParams;
        constants.envMapImportanceSamplingParams = m_envMapBaker->GetImportanceSampling()->GetShaderParams();

        PlanarViewConstants view;           m_renderCore.camera().view()->FillPlanarViewConstants(view);
        PlanarViewConstants previousView;   m_renderCore.camera().viewPrevious()->FillPlanarViewConstants(previousView);
        constants.view          = FromPlanarViewConstants(view);
        constants.previousView  = FromPlanarViewConstants(previousView);

        constants.debug = {};
        constants.debug.pick = m_pick || m_pickInstance || m_settings.ContinuousDebugFeedback;
        constants.debug.pickX = (constants.debug.pick)?(m_settings.DebugPixel.x):(-1);
        constants.debug.pickY = (constants.debug.pick)?(m_settings.DebugPixel.y):(-1);
        constants.debug.debugLineScale = (m_settings.ShowDebugLines)?(m_settings.DebugLineScale):(0.0f);
        constants.debug.showWireframe = m_settings.ShowWireframe;
        constants.debug.debugViewType = (int)m_settings.DebugView;
        constants.debug.debugViewStablePlaneIndex = (m_settings.StablePlanesActiveCount==1)?(0):(m_settings.DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
        constants.debug.exploreDeltaTree = (m_editor.ShowDeltaTree && constants.debug.pick)?(1):(0);
#else
        constants.debug.exploreDeltaTree = false;
#endif
        constants.debug.imageWidth = constants.ptConsts.imageWidth;
        constants.debug.imageHeight = constants.ptConsts.imageHeight;
        constants.debug.mouseX = m_settings.MousePos.x;
        constants.debug.mouseY = m_settings.MousePos.y;
        constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
        constants.debug._padding0 = 0;

        constants.denoisingHitParamConsts = { m_settings.ReblurSettings.hitDistanceParameters.A, m_settings.ReblurSettings.hitDistanceParameters.B,
                                              m_settings.ReblurSettings.hitDistanceParameters.C, m_settings.ReblurSettings.hitDistanceParameters.D };

        // This updates all lighting: distant (environment maps and directional analytic lights) and local (analytic lights and emissive triangle lights)
        // Must go before m_constantBuffer as when saving screenshots it closes and re-opens command list, flushing the volatile constant buffer!
        UpdateLighting(m_commandList);
        UploadSubInstanceData(m_commandList); // this is now full subInstance data

        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        SampleRenderCode(framebuffer, m_commandList, constants);

        const bool stochasticSplats = m_settings.EnableGaussianSplats && m_settings.GaussianSplatSortingMode == 1;
        const bool stochasticUsesMainTemporal = stochasticSplats && (!m_settings.RealtimeMode || m_settings.RealtimeAA == 1);
        if (stochasticUsesMainTemporal)
            RenderGaussianSplats(true);

        PostProcessAA(framebuffer, needNewPasses || m_settings.ResetRealtimeCaches);
        ApplyReferenceOIDN();
        if (m_settings.ReferenceOIDNDenoiser)
            m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        if (!stochasticUsesMainTemporal)
            RenderGaussianSplats(false);
    }

    caustica::PlanarView fullscreenView = *m_renderCore.camera().view();
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    PostProcessPreToneMapping(m_commandList, fullscreenView);   // writing to m_renderTargets->ProcessedOutputColor

    //Tone Mapping; it will read from m_renderTargets->ProcessedOutputColor and write into m_renderTargets->LdrColor; in case tonemapping is disabled, it's just a passthrough
    if (m_toneMappingPass->Render(m_commandList, fullscreenView, m_renderTargets->ProcessedOutputColor, m_settings.EnableToneMapping))
    {
        // first run tonemapper can close & re-open command list - when that happens, we have to re-upload volatile constants
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }

    PostProcessPostToneMapping(m_commandList, fullscreenView);  // writing to m_renderTargets->LdrColor

    //m_postProcess->Render(m_commandList, m_renderTargets->LdrColor);

    if (m_settings.EnableShaderDebug)
        m_shaderDebug->EndFrameAndOutput(m_commandList, m_renderTargets->LdrFramebuffer->GetFramebuffer(fullscreenView), m_renderTargets->Depth, fbinfo.getViewport());

    m_zoomTool->Render(m_commandList, m_renderTargets->LdrColor);

    m_commandList->beginMarker("Blit");
    m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->LdrColor, m_bindingCache.get());
    m_commandList->endMarker();

    if (m_settings.ShowDebugLines == true)
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

    if( m_settings.ContinuousDebugFeedback || m_pick )
    {
        m_commandList->copyBuffer(m_feedback_Buffer_Cpu, 0, m_feedback_Buffer_Gpu, 0, sizeof(DebugFeedbackStruct) * 1);
        m_commandList->copyBuffer(m_debugLineBufferDisplay, 0, m_debugLineBufferCapture, 0, sizeof(DebugLineStruct) * MAX_DEBUG_LINES );
        m_commandList->copyBuffer(m_debugDeltaPathTree_Cpu, 0, m_debugDeltaPathTree_Gpu, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
	}

    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;


	m_commandList->close();
	GetDevice()->executeCommandList(m_commandList);

    // resolve picking and debug info
    if (m_settings.ContinuousDebugFeedback || m_pick || m_pickInstance)
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
            m_editor.SelectedMaterial = FindMaterial(int(m_feedbackData.pickedMaterialID));

        if (m_pickInstance)
        {
            m_editor.SelectedNode = FindNodeByInstanceIndex(int(m_feedbackData.pickedInstanceIndex));
            if (m_editor.SelectedNode != nullptr)
                m_editor.SelectedGaussianSplat = false;
        }

        m_pick = false;
        m_pickInstance = false;
    }

    auto DumpScreenshot = [this, framebufferTexture](const char* fileName)
    {
        return SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, fileName);
    };

    m_captureScriptManager->PostRender(DumpScreenshot);

    if (m_editor.ExperimentalPhotoModeScreenshot)
    {
        DenoisedScreenshot( framebufferTexture );
        m_editor.ExperimentalPhotoModeScreenshot = false;
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->AdvanceFrame();

	m_renderCore.camera().swapViews();
	GetGpuDevice()->SetVsyncEnabled(m_settings.ActualEnableVsync());

    PostUpdatePathTracing();
}

void PathTracerApp::RecreateBindingSet()
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    nvrhi::rt::IAccelStruct* gaussianSplatAS = m_renderCore.accelStructs().getTopLevelAS();
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
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_renderCore.accelStructs().getTopLevelAS()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_renderCore.accelStructs().getSubInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_sceneManager->getScene()->GetInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_sceneManager->getScene()->GetGeometryBuffer()),
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

std::shared_ptr<caustica::Material> PathTracerApp::FindMaterial(int materialID) const
{
    return SceneManager::findMaterial(m_sceneManager->getScene(), materialID);
}

std::shared_ptr<caustica::SceneGraphNode> PathTracerApp::FindNodeByInstanceIndex(int instanceIndex) const
{
    return SceneManager::findNodeByInstanceIndex(m_sceneManager->getScene(), instanceIndex);
}


void PathTracerApp::HandleDroppedFiles()
{
    if (m_editor.PendingDroppedFiles.empty())
        return;

    auto files = std::move(m_editor.PendingDroppedFiles);
    m_editor.PendingDroppedFiles.clear();

    for (const auto& filePath : files)
    {
        std::filesystem::path path(filePath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        if (ext == ".ply")
        {
            caustica::info("Drag-drop: loading Gaussian Splat file '%s'", filePath.c_str());
            if (LoadGaussianSplatFile(path))
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects", GetGaussianSplatCount(), GetGaussianSplatObjectCount());
            else
                caustica::error("Failed to load Gaussian Splat file '%s'", filePath.c_str());
        }
        else if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
        {
            caustica::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (LoadMeshFile(path))
                caustica::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                caustica::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            caustica::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj)", ext.c_str());
        }
    }
}

bool PathTracerApp::LoadMeshFile(const std::filesystem::path& filePath)
{
    if (!m_sceneManager->getScene() || !m_shaderFactory || !m_TextureCache)
    {
        caustica::error("Cannot load mesh: scene, shader factory, or texture cache not initialized.");
        return false;
    }

    std::filesystem::path absPath = filePath;
    if (!absPath.is_absolute())
        absPath = std::filesystem::absolute(absPath);

    if (!std::filesystem::exists(absPath))
    {
        caustica::error("File does not exist: '%s'", absPath.string().c_str());
        return false;
    }

    std::string ext = absPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (ext == ".gltf" || ext == ".glb")
        return LoadGltfMeshFile(absPath);
    if (ext == ".obj")
        return LoadObjMeshFile(absPath);

    caustica::error("Unsupported mesh file type '%s'.", ext.c_str());
    return false;
}

bool PathTracerApp::LoadGltfMeshFile(const std::filesystem::path& filePath)
{
    auto fs = std::make_shared<caustica::NativeFileSystem>();
    auto sceneTypeFactory = std::make_shared<caustica::render::RenderSceneTypeFactory>();
    auto importer = std::make_shared<caustica::GltfImporter>(fs, sceneTypeFactory);

    caustica::SceneLoadingStats stats;
    caustica::SceneImportResult importResult;

    std::filesystem::path sceneDirectory;
    if (m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_sceneManager->getCurrentScenePath().parent_path();

    if (!importer->Load(filePath, *m_TextureCache, stats, nullptr, importResult, sceneDirectory))
    {
        caustica::error("GltfImporter failed to load '%s'", filePath.string().c_str());
        return false;
    }

    if (!importResult.rootNode)
    {
        caustica::error("GltfImporter produced no root node for '%s'", filePath.string().c_str());
        return false;
    }

    importResult.rootNode->SetName(filePath.stem().string());

    auto importedRoot = m_sceneManager->getScene()->GetSceneGraph()->Attach(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}


bool PathTracerApp::LoadObjMeshFile(const std::filesystem::path& filePath)
{
    auto sceneTypeFactory = std::make_shared<caustica::render::RenderSceneTypeFactory>();
    caustica::ObjImporter importer(sceneTypeFactory);

    caustica::SceneLoadingStats stats;
    SceneImportResult importResult;
    if (!importer.Load(filePath, *m_TextureCache, stats, nullptr, importResult))
        return false;

    if (!importResult.rootNode)
        return false;

    auto importedRoot = m_sceneManager->getScene()->GetSceneGraph()->Attach(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(importedRoot);

    return true;
}

void PathTracerApp::FinalizeRuntimeSceneMutation(const std::shared_ptr<caustica::SceneGraphNode>& importedRoot)
{
    if (importedRoot)
    {
        std::unordered_set<caustica::Material*> processedMaterials;
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

    m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());

    if (m_materialsBaker != nullptr)
        m_materialsBaker->SceneReloaded();
    if (m_lightsBaker != nullptr)
        m_lightsBaker->SceneReloaded();
    if (m_ommBaker != nullptr)
        m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    RequestFullRebuild();
}

bool PathTracerApp::DeleteSceneNode(const std::shared_ptr<SceneGraphNode>& node)
{
    if (node == nullptr || m_sceneManager->getScene() == nullptr)
        return false;

    auto sceneGraph = m_sceneManager->getScene()->GetSceneGraph();
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
    m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());

    m_lights.clear();
    for (auto light : sceneGraph->GetLights())
        m_lights.push_back(light);

    if (m_editor.TogglableNodes != nullptr)
    {
        m_editor.TogglableNodes->clear();
        UpdateTogglableNodes(*m_editor.TogglableNodes, rootNode.get());
    }

    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedNode = nullptr;
    m_editor.InspectorRotationNode.reset();
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;

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
        m_ommBaker->SceneLoaded(*m_sceneManager->getScene());

    RequestFullRebuild();
    return true;
}


void PathTracerApp::RequestFullRebuild()
{
    m_editor.AccelerationStructRebuildRequested = true;
    m_editor.ShaderReloadRequested = true;
    m_editor.ShaderAndACRefreshDelayedRequest = 0.0f;
    m_settings.ResetAccumulation = true;
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

std::vector<float3> PathTracerApp::GetMeshVertices(const std::shared_ptr<MeshInfo>& mesh) const
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

std::vector<float3> PathTracerApp::GetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_sceneManager->getScene(), mesh, "GetMeshVerticesWorld");
    return GetMeshVerticesWorld(node);
}

std::vector<float3> PathTracerApp::GetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node)
{
    if (!m_sceneManager->getScene())
        throw std::runtime_error("GetMeshVerticesWorld: no scene is loaded");

    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "GetMeshVerticesWorld");
    std::vector<float3> vertices = GetMeshVertices(mesh);

    const affine3 localToWorld = node->GetLocalToWorldTransformFloat();
    for (float3& vertex : vertices)
        vertex = localToWorld.transformPoint(vertex);

    return vertices;
}

void PathTracerApp::SetMeshVerticesWorld(const std::shared_ptr<MeshInfo>& mesh,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    auto node = FindUniqueMeshInstanceNodeForDeform(m_sceneManager->getScene(), mesh, "SetMeshVerticesWorld");
    SetMeshVerticesWorld(node, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void PathTracerApp::SetMeshVerticesWorld(const std::shared_ptr<SceneGraphNode>& node,
                                  const std::vector<float3>& vertices,
                                  bool recomputeNormals,
                                  bool rebuildAccelerationStructure)
{
    if (!m_sceneManager->getScene())
        throw std::runtime_error("SetMeshVerticesWorld: no scene is loaded");

    m_sceneManager->getScene()->RefreshSceneGraph(GetFrameIndex());

    auto mesh = GetMeshFromSceneNodeForDeform(node, "SetMeshVerticesWorld");
    const affine3 worldToLocal = affine3(inverse(node->GetLocalToWorldTransform()));

    std::vector<float3> objectVertices;
    objectVertices.reserve(vertices.size());
    for (const float3& vertex : vertices)
    {
        const float3 objectVertex = worldToLocal.transformPoint(vertex);
        if (!dm::all(dm::isfinite(objectVertex)))
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

void PathTracerApp::SetMeshVertices(const std::shared_ptr<MeshInfo>& mesh,
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
        if (m_sceneManager->getScene() && m_sceneManager->getScene()->GetSceneGraph())
        {
            for (const auto& instance : m_sceneManager->getScene()->GetSceneGraph()->GetMeshInstances())
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

        if (m_sceneManager->getScene())
            m_sceneManager->getScene()->FinishedLoading(GetFrameIndex());
    }

    if (rebuildAccelerationStructure)
        RequestMeshAccelRebuild(mesh);
    else
        m_settings.ResetAccumulation = true;
}

void PathTracerApp::PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants)
{
    //m_commandList->beginMarker("MainRendering"); <- removed (for now) since added hierarchy reduces readability

    bool useStablePlanes = m_settings.RealtimeMode;

    nvrhi::rt::State state;

    nvrhi::rt::DispatchRaysArguments args;
    nvrhi::Viewport viewport = m_renderCore.camera().view()->GetViewport();
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
    m_lightsBaker->UpdateEnd(m_commandList, *m_bindingCache, m_sceneManager->getScene(), m_materialsBaker, m_ommBaker, m_renderCore.accelStructs().getSubInstanceBuffer(), m_renderTargets->Depth, m_renderTargets->ScreenMotionVectors);  // <- in the future this will provide motion vectors except in case of reference mode

    {
        RAII_SCOPE( m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

        state.shaderTable = ((useStablePlanes) ? (m_ptPipelineFillStablePlanes) : (m_ptPipelineReference))->GetShaderTable();
        state.bindings = { m_bindingSet, m_DescriptorTable->GetDescriptorTable() };

        for (uint subSampleIndex = 0; subSampleIndex < m_settings.ActualSamplesPerPixel(); subSampleIndex++)
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
    bool useFusedDIGIFinal = m_settings.ActualUseReSTIRDI() && m_settings.ActualUseReSTIRGI() && enableFusedDIGIFinal;

    if (m_settings.ActualUseRTXDIPasses())
    {
        RAII_SCOPE( m_commandList->beginMarker("RTXDI");, m_commandList->endMarker(); );

        // this does all ReSTIR DI magic including applying the final sample into correct radiance buffer (depending on denoiser state)
        if (m_settings.ActualUseReSTIRDI())
            m_rtxdiPass->Execute(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (m_settings.ActualUseReSTIRGI())
            m_rtxdiPass->ExecuteGI(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (useFusedDIGIFinal)
            m_rtxdiPass->ExecuteFusedDIGIFinal(m_commandList, m_bindingSet);

        if (m_settings.ActualUseReSTIRPT())
            m_rtxdiPass->ExecutePT(m_commandList, m_bindingSet);
    }

    {
        RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

        m_denoisingGuidesBaker->DenoiseSpecHitT(m_commandList, m_bindingSet);
        m_denoisingGuidesBaker->ComputeAvgLayerRadiance(m_commandList, m_bindingSet);

        if (m_settings.DebugView != DebugViewType::Disabled)
            m_denoisingGuidesBaker->RenderDebugViz(m_commandList, m_settings.DebugView, m_bindingSet);
    }

    if (useStablePlanes && (m_settings.DebugView > DebugViewType::Disabled && m_settings.DebugView <= DebugViewType::StablePlane_SpecRadiance || m_settings.DebugView == DebugViewType::StableRadiance) )
    {
        m_commandList->beginMarker("StablePlanesDebugViz");
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::StablePlanesDebugViz, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

    }
}

void PathTracerApp::Denoise(nvrhi::IFramebuffer* framebuffer)
{
    if( !m_settings.ActualUseStandaloneDenoiser() )
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] == nullptr)
        {
            nrd::Denoiser denoiserMethod = m_settings.NRDMethod == NrdConfig::DenoiserMethod::REBLUR ?
                nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

            m_nrd[i] = std::make_unique<NrdIntegration>(GetDevice(), denoiserMethod);
            m_nrd[i]->Initialize(m_renderSize.x, m_renderSize.y, *m_shaderFactory);
        }
    }

    //const auto& fbinfo = framebuffer->getFramebufferInfo();
    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" }; assert( std::size(m_nrd) <= std::size(passNames) );

    bool nrdUseRelax = m_settings.NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    PostProcess::ComputePassType preparePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    PostProcess::ComputePassType mergePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    bool resetHistory = m_settings.ResetRealtimeCaches;

    int maxPassCount = std::min(m_settings.StablePlanesActiveCount, (int)std::size(m_nrd));
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
        bool enableValidation = m_settings.DebugView == DebugViewType::StablePlane_DenoiserValidation;
        if (nrdUseRelax)
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_renderCore.camera().view(), *m_renderCore.camera().viewPrevious(), GetFrameIndex(), m_settings.NRDDisocclusionThreshold, m_settings.NRDDisocclusionThresholdAlternate, m_settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_settings.RelaxSettings);
        }
        else
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_renderCore.camera().view(), *m_renderCore.camera().viewPrevious(), GetFrameIndex(), m_settings.NRDDisocclusionThreshold, m_settings.NRDDisocclusionThresholdAlternate, m_settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_settings.ReblurSettings);
        }

        m_commandList->beginMarker("MergeOutputs");
        m_postProcess->Apply(m_commandList, mergePassType, pass, m_constantBuffer, miniConstants, m_renderTargets->OutputColor, *m_renderTargets, nullptr);
        m_commandList->endMarker();

        m_commandList->endMarker();
    }
}

#if CAUSTICA_WITH_NATIVE_DLSS
bool PathTracerApp::EvaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_settings.RealtimeAA == 3;
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

    caustica::render::DLSS::InitParameters initParams;
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

    caustica::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_renderTargets->OutputColor;
    evaluateParams.outputColorTexture = m_renderTargets->ProcessedOutputColor;
    evaluateParams.depthTexture = m_renderTargets->Depth;
    evaluateParams.motionVectorsTexture = m_renderTargets->ScreenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_renderSize.y);
    evaluateParams.resetHistory = reset || m_settings.ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->RRDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->RRSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->RRNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->Evaluate(m_commandList, evaluateParams, *m_renderCore.camera().view());
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            caustica::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_renderSize.x, m_renderSize.y, m_displaySize.x, m_displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif

void PathTracerApp::PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    (void)framebuffer;

    PostProcessAAParams params{
        m_settings,
        m_commandList,
        m_renderTargets.get(),
        GetGpuDevice(),
    };
    params.renderSize = m_renderSize;
    params.displaySize = m_displaySize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.cameraJitter = ComputeCameraJitter(m_sampleIndex);
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = GetFrameIndex();
    params.reset = reset;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    params.accumulationPass = m_accumulationPass.get();
    params.postProcess = m_postProcess.get();
    params.bindingSet = m_bindingSet;
    params.bindingLayout = m_bindingLayout;
    params.constantBuffer = m_constantBuffer;
    params.accumulationSampleIndex = m_accumulationSampleIndex;
    params.gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex;
    params.gaussianSplatTemporalReset = &m_gaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = &m_lastDLSSRROptions;
#endif

    m_renderCore.postProcessAA(params);

#if CAUSTICA_WITH_NATIVE_DLSS
    if (m_settings.RealtimeMode)
    {
        bool nativeDLSSEvaluated = false;
        if (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3)
            nativeDLSSEvaluated = EvaluateNativeDLSS(reset);

        if (!nativeDLSSEvaluated && (m_settings.RealtimeAA == 2 || m_settings.RealtimeAA == 3))
        {
            if (m_settings.ActualUseStandaloneDenoiser())
            {
                m_commandList->copyTexture(
                    m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(),
                    m_renderTargets->OutputColor, nvrhi::TextureSlice());
            }
            else
            {
                SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
                m_commandList->beginMarker("NoDenoiserFinalMerge");
                m_postProcess->Apply(
                    m_commandList,
                    PostProcess::ComputePassType::NoDenoiserFinalMerge,
                    m_constantBuffer,
                    miniConstants,
                    m_bindingSet,
                    m_bindingLayout,
                    tdesc.width,
                    tdesc.height);
                m_commandList->endMarker();
            }
        }
    }
#endif
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

void PathTracerApp::ResetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->Reset();
}

void PathTracerApp::ApplyReferenceOIDN()
{
    if (m_settings.RealtimeMode || !m_settings.ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_settings.AccumulationTarget;
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
        caustica::warning("OIDN reference denoiser expected RGBA32_FLOAT accumulation buffer, got %s.", nvrhi::utils::FormatToString(sourceDesc.format));
        m_oidnDenoiserFailed = true;
        return;
    }

    const uint32_t width = sourceDesc.width;
    const uint32_t height = sourceDesc.height;

    OidnDenoiser::Options oidnOptions;
    oidnOptions.UseGPU = m_settings.ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_settings.ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_settings.ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_settings.ReferenceOIDNQuality, 0, 2));

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
        caustica::warning("OIDN reference denoiser failed to create a staging texture.");
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
        caustica::warning("OIDN reference denoiser readback failed because the GPU device was lost.");
        m_oidnDenoiserFailed = true;
        return;
    }

    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(GetDevice()->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
    {
        m_commandList->open();
        caustica::warning("OIDN reference denoiser failed to map the accumulation buffer.");
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
        caustica::warning("OIDN reference denoiser failed: %s", m_oidnDenoiser->GetLastError().c_str());
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

    caustica::info("OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->GetDeviceDescription().c_str(), width, height);
#else
    if (!m_oidnDenoiserFailed)
    {
        caustica::warning("OIDN reference denoiser requested, but CAUSTICA_WITH_OIDN is disabled in this build.");
        m_oidnDenoiserFailed = true;
    }
#endif
}

void PathTracerApp::DenoisedScreenshot(nvrhi::ITexture * framebufferTexture) const
{
    std::string noisyImagePath = (caustica::GetDirectoryWithExecutable( ) / "photo.bmp").string();

    auto execute = [&](const std::string & dn = "OptiX")
    {
	    const auto p1 = std::chrono::system_clock::now();
		const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count());

        const std::string fileName = "photo-denoised_" + dn + "_" + timestamp + ".bmp";

        std::string denoisedImagePath = (caustica::GetDirectoryWithExecutable() / fileName).string();
        std::filesystem::path denoiserPath = GetLocalPath("Support/denoiser_"+dn) / "denoiser.exe";
        if (!std::filesystem::exists(denoiserPath))
        {
            caustica::warning("External %s denoiser not found at '%s'.", dn.c_str(), denoiserPath.string().c_str());
            return;
        }

        if (!SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, noisyImagePath.c_str()))
        { assert(false); return; }

        std::string startCmd = "\"" + denoiserPath.string() + "\"" + " -hdr 0 -i \"" + noisyImagePath + "\"" " -o \"" + denoisedImagePath + "\"";
        auto [resNum, resString, errorString] =  SystemShell(startCmd.c_str());
        if (resString!="")
            caustica::info("result: %s", resString.c_str());
        if (errorString != "")
            caustica::info("error: %s", errorString.c_str());

        std::string viewCmd = "\"" + denoisedImagePath + "\"";
        SystemShell(viewCmd.c_str(), true);
    };
    execute("OptiX");
    execute("OIDN");
}

dm::float2 PathTracerApp::ComputeCameraJitter(uint frameIndex)
{
    return m_renderer->computeCameraJitter(frameIndex);
}
