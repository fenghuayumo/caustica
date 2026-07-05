#define USE_DENOISING_NRD 1     // <- this define should come from elsewhere

#include <shaders/PathTracer/Config.h>

#include <render/Core/RenderTargets.h>

#include <render/Core/FramebufferFactory.h>

#include <math/math.h>
using namespace caustica::math;

#include <shaders/PathTracer/StablePlanes.hlsli>

#include <shaders/render/RTXDI/ShaderParameters.h>
#include <core/log.h>

#include <cmath>
#include <algorithm>
#include <cstdint>

using namespace dm;
using namespace caustica::math;

void RenderTargets::init(
        nvrhi::IDevice* device,
        dm::uint2 renderSize, 
        dm::uint2 displaySize,
        bool enableMotionVectors,
        bool useReverseProjection,
        int backbufferCount
    )
{
    m_useReverseProjection = useReverseProjection;
    m_backbufferCount = backbufferCount;
    m_device = device;
    this->renderSize = renderSize;
    this->displaySize = displaySize;

    nvrhi::TextureDesc desc;
    desc.width = renderSize.x;
    desc.height = renderSize.y;

    desc.isVirtual = false; //device->queryFeatureSupport(nvrhi::Feature::VirtualResources); <- codepath not up to date, needs refactoring

    desc.sampleCount = 1; assert(m_sampleCount == 1);
    desc.dimension =  nvrhi::TextureDimension::Texture2D;
    desc.keepInitialState = true;
    desc.mipLevels = 1;

    desc.format = nvrhi::Format::R32_FLOAT;
    desc.isTypeless = false;
    desc.isUAV = true;
    desc.isRenderTarget = false;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.debugName = "depth";
    //desc.clearValue = useReverseProjection ? nvrhi::Color(0.f) : nvrhi::Color(1.f);
    //desc.useClearValue = true;
    depth = device->createTexture(desc);

    desc.isTypeless = false;
    desc.isRenderTarget = false;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.clearValue = nvrhi::Color(0.f);
    desc.isUAV = true;
    desc.format = nvrhi::Format::RGBA16_FLOAT;	// allow for .z component too
    desc.isRenderTarget = true;
    desc.debugName = "screenMotionVectors";
    screenMotionVectors = device->createTexture(desc);
    desc.isRenderTarget = false;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "denoiserMotionVectors";
    denoiserMotionVectors = device->createTexture(desc);

    desc.format = nvrhi::Format::R32_UINT;
    desc.debugName = "throughput";
    throughput = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "StableRadianceBuffer";
    stableRadiance = device->createTexture(desc);
    
    desc.format = nvrhi::Format::R32_UINT;
    desc.arraySize = 4;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.debugName = "stablePlanesHeader";
    stablePlanesHeader = device->createTexture(desc);
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.arraySize = 1;

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "denoiserDiffRadianceHitDist";
    denoiserDiffRadianceHitDist = device->createTexture(desc);
    desc.debugName = "denoiserOutDiffRadianceHitDist";
    for (int i = 0; i < cStablePlaneCount; ++i)
        denoiserOutDiffRadianceHitDist[i] = device->createTexture(desc);

    desc.debugName = "denoiserSpecRadianceHitDist";
    denoiserSpecRadianceHitDist = device->createTexture(desc);
    desc.debugName = "denoiserOutSpecRadianceHitDist";
    for (int i = 0; i < cStablePlaneCount; ++i)
        denoiserOutSpecRadianceHitDist[i] = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.clearValue = nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f);   // avoid the debug layer warnings... not actually cleared except for debug purposes
#if ENABLE_DEBUG_VIZUALISATIONS
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = "denoiserOutValidation";
    denoiserOutValidation = device->createTexture(desc);
#endif

    desc.format = nvrhi::Format::R32_FLOAT;
    desc.debugName = "denoiserViewspaceZ";
    denoiserViewspaceZ = device->createTexture(desc);

    desc.format = nvrhi::Format::R10G10B10A2_UNORM;
    desc.debugName = "denoiserNormalRoughness";
    denoiserNormalRoughness = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.debugName = "secondarySurfacePositionNormal";
    secondarySurfacePositionNormal = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "secondarySurfaceRadiance";
    secondarySurfaceRadiance = device->createTexture(desc);

    desc.format = nvrhi::Format::R32_FLOAT;   // currently shaders expect FP32 but could be tweaked to FP16
    desc.debugName = "specularHitT";
    specularHitT = device->createTexture(desc);

    desc.debugName = "scratchFloat1";
    scratchFloat1 = device->createTexture(desc);
    
    desc.isUAV = false;
    desc.isRenderTarget = true;
    desc.useClearValue = true;
    desc.clearValue = nvrhi::Color(1.f);
    desc.sampleCount = m_sampleCount;
    desc.dimension = m_sampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
    desc.keepInitialState = true;

    desc.useClearValue = false;
    desc.clearValue = nvrhi::Color(0.f);
    desc.isTypeless = false;
    desc.isUAV = true;
    desc.isRenderTarget = true;
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.debugName = "accumulatedRadiance";
    accumulatedRadiance = device->createTexture(desc);

#if 0
    nvrhi::Format radianceFormat = nvrhi::Format::R11G11B10_FLOAT;
#else
    nvrhi::Format radianceFormat = nvrhi::Format::RGBA16_FLOAT;
#endif

    desc.useClearValue = true;
    desc.format = radianceFormat;  // keep in float for now in case we need 
    desc.debugName = "outputColor";
    desc.clearValue = nvrhi::Color(1.0f, 1.0f, 0.0f, 0.0f);   // avoid the debug layer warnings... not actually cleared except for debug purposes
    desc.isUAV = true;
    outputColor = device->createTexture(desc);

    desc.format = nvrhi::Format::R8_UNORM;
    desc.isUAV = true;
    desc.debugName = "denoiserDisocclusionThresholdMix";
    denoiserDisocclusionThresholdMix = device->createTexture(desc);
    desc.debugName = "combinedHistoryClampRelax";
    combinedHistoryClampRelax = device->createTexture(desc);

    // these are used for DLSS-RR - we could overlap with other buffers to save on RAM but we leave as separate for simplicity
    // see https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md
    desc.format = nvrhi::Format::R11G11B10_FLOAT;       // from docs: "Diffuse component of Reflectance material. Any standard 3-channel format provided at input resolution. The format must be linear, sRGB textures are not supported."
    desc.debugName = "rrDiffuseAlbedo";
    rrDiffuseAlbedo = device->createTexture(desc);
    desc.debugName = "rrSpecAlbedo";
    rrSpecAlbedo = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "rrNormalsAndRoughness";
    rrNormalsAndRoughness = device->createTexture(desc);
    desc.format = nvrhi::Format::RG16_FLOAT;
    desc.debugName = "rrSpecMotionVectors";
    rrSpecMotionVectors = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "rrTransparencyLayer";
    // rrTransparencyLayer = device->createTexture(desc); // not currently used

    // GBuffer targets are kept as placeholders for the main path-tracing renderer.
    renderSize.x = 1;
    renderSize.y = 1;

    desc.debugName = "GBufferBaseColor"; // Can reuse rrDiffuseAlbedo?
    desc.format = nvrhi::Format::R11G11B10_FLOAT;
    baseColor = device->createTexture(desc);
    desc.debugName = "GBufferSpecNormal";
    desc.format = nvrhi::Format::R32_UINT;
    specNormal = device->createTexture(desc);
    desc.debugName = "GBufferRoughnessMetal";
    desc.format = nvrhi::Format::RG16_FLOAT;
    roughnessMetal = device->createTexture(desc);
    desc.debugName = "GBufferMaterialInfo";
    desc.format = nvrhi::Format::R32_UINT;
    materialInfo = device->createTexture(desc);

    // !!! NOTE !!! setting desc.width/desc.height to half render size (was render size!)
    desc.width = (renderSize.x + 1) / 2;
    desc.height = (renderSize.y + 1) / 2;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "DenoiserAvgRadianceHalfRes";
    denoiserAvgLayerRadianceHalfRes = device->createTexture(desc);

    // !!! NOTE !!! setting desc.width/desc.height to display size (was render size!)
    desc.width = displaySize.x;
    desc.height = displaySize.y;

    desc.format = radianceFormat;
    desc.debugName = "processedOutputColor";
    processedOutputColor = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_SNORM;
    desc.debugName = "temporalFeedback1";
    temporalFeedback1 = device->createTexture(desc);
    desc.debugName = "temporalFeedback2";
    temporalFeedback2 = device->createTexture(desc);

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.isUAV = true;
    desc.isTypeless = true;
    desc.debugName = "ldrColor";
    ldrColor = device->createTexture(desc);
    desc.debugName = "ldrColorScratch";
    ldrColorScratch = device->createTexture(desc);
    desc.isTypeless = false;

    desc.isUAV = false;

    desc.debugName = "preUIColor";
    preUIColor = device->createTexture(desc);

#if 0
    if (desc.isVirtual)
    {
        uint64_t heapSize = 0;
        nvrhi::ITexture* const textures[] = {
            //HdrColor,
            //ResolvedColor,
            //temporalFeedback1,
            //temporalFeedback2,
            ldrColor,
            outputColor,
            preUIColor,
            //AmbientOcclusion
        };

        for (auto texture : textures)
        {
            nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
            heapSize = nvrhi::align(heapSize, memReq.alignment);
            heapSize += memReq.size;
        }

        nvrhi::HeapDesc heapDesc;
        heapDesc.type = nvrhi::HeapType::DeviceLocal;
        heapDesc.capacity = heapSize;
        heapDesc.debugName = "RenderTargetHeap";

        heap = device->createHeap(heapDesc);

        uint64_t offset = 0;
        for (auto texture : textures)
        {
            nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
            offset = nvrhi::align(offset, memReq.alignment);

            device->bindTextureMemory(texture, heap, offset);

            offset += memReq.size;
        }
    }
#endif

    // === Reflection System Render Targets ===
    {
        // Local cubemap for ray-traced environment probe
        nvrhi::TextureDesc cubeDesc;
        cubeDesc.width = LocalCubemapSize;
        cubeDesc.height = LocalCubemapSize;
        cubeDesc.depth = 1;
        cubeDesc.arraySize = 6;
        cubeDesc.mipLevels = getNumMipLevels(LocalCubemapSize, LocalCubemapSize);
        cubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
        cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
        cubeDesc.debugName = "localCubemap";
        cubeDesc.isUAV = true;
        cubeDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        cubeDesc.keepInitialState = true;
        localCubemap = device->createTexture(cubeDesc);
        
        // SSR result (screen-sized, with alpha for confidence)
        nvrhi::TextureDesc ssrDesc;
        ssrDesc.width = renderSize.x;
        ssrDesc.height = renderSize.y;
        ssrDesc.format = nvrhi::Format::RGBA16_FLOAT;
        ssrDesc.dimension = nvrhi::TextureDimension::Texture2D;
        ssrDesc.debugName = "ssrResult";
        ssrDesc.isUAV = true;
        ssrDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        ssrDesc.keepInitialState = true;
        ssrResult = device->createTexture(ssrDesc);
        
        // SSR blur mip chain
        ssrDesc.mipLevels = getNumMipLevels(renderSize.x, renderSize.y);
        ssrDesc.debugName = "ssrBlurMipChain";
        ssrBlurMipChain = device->createTexture(ssrDesc);
    }

    outputFramebuffer = std::make_shared<caustica::FramebufferFactory>(device);
    outputFramebuffer->renderTargets = { outputColor };

    processedOutputFramebuffer = std::make_shared<caustica::FramebufferFactory>(device);
    processedOutputFramebuffer->renderTargets = { processedOutputColor };

    ldrFramebuffer = std::make_shared<caustica::FramebufferFactory>(device);
    ldrFramebuffer->renderTargets = { ldrColor };

    { // Stable planes
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.isVertexBuffer = false;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;

        bufferDesc.structStride = sizeof(StablePlane);
        bufferDesc.byteSize = sizeof(StablePlane) * GenericTSComputeStorageElementCount(this->renderSize.x, this->renderSize.y, cStablePlaneCount);
        bufferDesc.debugName = "stablePlanesBuffer";
        stablePlanesBuffer = device->createBuffer(bufferDesc);
    }

    {
        nvrhi::BufferDesc surfaceBufferDesc;
        surfaceBufferDesc.byteSize = sizeof(PackedPathTracerSurfaceData) * 2 * renderSize.x * renderSize.y; // *2 is for history!
        surfaceBufferDesc.byteSize = sizeof(PackedPathTracerSurfaceData) * GenericTSComputeStorageElementCount(this->renderSize.x, this->renderSize.y, 2); // *2 is for history!
        surfaceBufferDesc.structStride = sizeof(PackedPathTracerSurfaceData);
        surfaceBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        surfaceBufferDesc.keepInitialState = true;
        surfaceBufferDesc.debugName = "SurfaceData(GBuffer)";
        surfaceBufferDesc.canHaveUAVs = true;
        surfaceDataBuffer = device->createBuffer(surfaceBufferDesc);
    }
}

[[nodiscard]] bool RenderTargets::isUpdateRequired(dm::uint2 renderSize, dm::uint2 displaySize, dm::uint sampleCount) const
{
    if (any(renderSize != this->renderSize) || any(displaySize != this->displaySize) || m_sampleCount != sampleCount) return true;
    return false;
}

void RenderTargets::clear(nvrhi::ICommandList* commandList) 
{
    const nvrhi::FormatInfo& depthFormatInfo = nvrhi::getFormatInfo(depth->getDesc().format);

    float depthClearValue = m_useReverseProjection ? 0.f : 1.f;
    commandList->clearTextureFloat(depth, nvrhi::AllSubresources, nvrhi::Color(depthClearValue));

    commandList->clearTextureFloat(combinedHistoryClampRelax, nvrhi::AllSubresources, nvrhi::Color(0));
}

uint32_t RenderTargets::getNumMipLevels(uint32_t width, uint32_t height)
{
    uint32_t maxDim = std::max(width, height);
    uint32_t levelsNum = 1 + (uint32_t)std::log2((float)maxDim);

    return levelsNum;
}
