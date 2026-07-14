#include <render/passes/gaussian/GaussianSplatPass.h>
#include <render/passes/gaussian/GaussianSplatGeometry.h>
#include <render/passes/gaussian/GaussianSplatSorter.h>
#include <render/passes/gaussian/GaussianSplatSorter.h>
#include <render/passes/gaussian/GaussianSplatSorter.h>
#include <backend/ViewRhiConversion.h>

#include <render/gpuSort/GPUSort.h>
#include <render/core/RenderTargets.h>

#include <core/log.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <scene/View.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <scene/loader/GaussianSplatLoader.h>
#include <scene/GaussianSplatData.h>

using namespace caustica::math;

namespace
{
    float Clamp01(float value)
    {
        return std::min(1.0f, std::max(0.0f, value));
    }
    SimpleViewConstants FromPlanarViewConstants(const PlanarViewConstants& view)
    {
        SimpleViewConstants ret;
        ret.matWorldToView = view.matWorldToView;
        ret.matViewToClip = view.matViewToClip;
        ret.matWorldToClipNoOffset = view.matWorldToClipNoOffset;
        ret.matClipToWorldNoOffset = view.matClipToWorldNoOffset;
        ret.matWorldToClip = view.matWorldToClip;
        ret.clipToWindowBias = view.clipToWindowBias;
        ret.clipToWindowScale = view.clipToWindowScale;
        ret.viewportOrigin = view.viewportOrigin;
        ret.viewportSize = view.viewportSize;
        ret.viewportSizeInv = view.viewportSizeInv;
        ret.pixelOffset = view.pixelOffset;
        return ret;
    }

    uint32_t FormatElementSize(GaussianSplatStorageFormat format)
    {
        switch (format)
        {
        case GaussianSplatStorageFormat::Float32:
            return sizeof(float);
        case GaussianSplatStorageFormat::Float16:
            return sizeof(uint16_t);
        case GaussianSplatStorageFormat::Uint8:
            return sizeof(uint8_t);
        default:
            return sizeof(float);
        }
    }

    uint8_t QuantizeUnorm8(float value)
    {
        return uint8_t(std::clamp(std::round(Clamp01(value) * 255.0f), 0.0f, 255.0f));
    }

    uint8_t QuantizeSnormRange8(float value, float minValue, float maxValue)
    {
        const float normalized = std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
        return uint8_t(std::clamp(std::round(normalized * 255.0f), 0.0f, 255.0f));
    }

    void StoreFormattedScalar(std::vector<uint8_t>& data, uint64_t scalarIndex, GaussianSplatStorageFormat format, float value, bool signedRange)
    {
        const uint64_t byteOffset = scalarIndex * FormatElementSize(format);
        switch (format)
        {
        case GaussianSplatStorageFormat::Float32:
        {
            std::memcpy(data.data() + byteOffset, &value, sizeof(value));
            break;
        }
        case GaussianSplatStorageFormat::Float16:
        {
            const float16_t halfValue = Float32ToFloat16(value);
            std::memcpy(data.data() + byteOffset, &halfValue.bits, sizeof(halfValue.bits));
            break;
        }
        case GaussianSplatStorageFormat::Uint8:
        {
            const uint8_t quantized = signedRange
                ? QuantizeSnormRange8(value, -1.0f, 1.0f)
                : QuantizeUnorm8(value);
            data[byteOffset] = quantized;
            break;
        }
        }
    }

    float ShCoefficientAt(const std::vector<float4>& packedCoefficients, uint32_t splatIndex, uint32_t scalarIndex)
    {
        const uint32_t float4Index = splatIndex * GAUSSIAN_SPLAT_SH_FLOAT4_COUNT + scalarIndex / 4u;
        if (float4Index >= packedCoefficients.size())
            return 0.0f;

        const float4 value = packedCoefficients[float4Index];
        switch (scalarIndex & 3u)
        {
        case 0: return value.x;
        case 1: return value.y;
        case 2: return value.z;
        default: return value.w;
        }
    }

    uint64_t AlignRawBufferSize(uint64_t size)
    {
        return std::max<uint64_t>(4, (size + 3u) & ~uint64_t(3u));
    }
}

GaussianSplatPass::GaussianSplatPass(
    nvrhi::IDevice* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
    , m_accelBuilder(device)
{
    m_constantBuffer = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GaussianSplatConstants), "GaussianSplatConstants", 16));

    nvrhi::BindingLayoutDesc rasterRenderLayoutDesc;
    rasterRenderLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    rasterRenderLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(1),
        nvrhi::BindingLayoutItem::RawBuffer_SRV(2),
        nvrhi::BindingLayoutItem::RawBuffer_SRV(3),
        nvrhi::BindingLayoutItem::Texture_SRV(4)
    };
    m_rasterRenderBindingLayout = m_device->createBindingLayout(rasterRenderLayoutDesc);

    nvrhi::BindingLayoutDesc hybridRenderLayoutDesc = rasterRenderLayoutDesc;
    hybridRenderLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::RayTracingAccelStruct(5));
    m_hybridRenderBindingLayout = m_device->createBindingLayout(hybridRenderLayoutDesc);

    nvrhi::BindingLayoutDesc sortLayoutDesc;
    sortLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    sortLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(1),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(2),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(3)
    };
    m_sortKeyBindingLayout = m_device->createBindingLayout(sortLayoutDesc);
}

void GaussianSplatPass::setGpuSort(std::shared_ptr<GPUSort> gpuSort)
{
    m_gpuSort = std::move(gpuSort);
}

caustica::render::GaussianSplatSortResources GaussianSplatPass::makeSortResources() const
{
    caustica::render::GaussianSplatSortResources resources;
    resources.sortKeyBuffer = m_sortKeyBuffer.Get();
    resources.indexBuffer = m_indexBuffer.Get();
    resources.sortControlBuffer = m_sortControlBuffer.Get();
    resources.drawIndirectBuffer = m_drawIndirectBuffer.Get();
    resources.sortKeyBindingSet = m_sortKeyBindingSet;
    resources.sortKeyPipeline = m_sortKeyPipeline;
    resources.gpuSort = m_gpuSort;
    resources.splatCount = m_splatCount;
    return resources;
}

bool GaussianSplatPass::loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    caustica::GaussianSplatDataset dataset;
    if (!caustica::loadGaussianSplatPly(fileName, convertRdfToRub, dataset))
        return false;

    m_splats = std::move(dataset.splats);
    m_shCoefficients = std::move(dataset.shCoefficients);
    m_emissionProxies.clear();
    m_splatCount = uint32_t(m_splats.size());
    m_shDegree = dataset.shDegree;
    m_colorOpacity.clear();
    m_colorOpacity.reserve(m_splats.size());
    for (const caustica::GaussianSplatData& splat : m_splats)
        m_colorOpacity.push_back(float4(splat.color.x, splat.color.y, splat.color.z, splat.centerOpacity.w));

    m_localBounds = box3::empty();
    for (const caustica::GaussianSplatData& splat : m_splats)
        m_localBounds |= splat.centerOpacity.xyz();
    m_localBoundsValid = !m_localBounds.isempty();

    if (m_shCoefficients.empty())
        m_shCoefficients.push_back(float4(0.0f, 0.0f, 0.0f, 0.0f));

    nvrhi::BufferDesc splatBufferDesc;
    splatBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(caustica::GaussianSplatData);
    splatBufferDesc.structStride = sizeof(caustica::GaussianSplatData);
    splatBufferDesc.debugName = "GaussianSplatDataBuffer";
    splatBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    splatBufferDesc.keepInitialState = true;
    m_splatBuffer = m_device->createBuffer(splatBufferDesc);

    m_colorBuffer = nullptr;
    m_shBuffer = nullptr;

    nvrhi::BufferDesc uintBufferDesc;
    uintBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(uint32_t);
    uintBufferDesc.format = nvrhi::Format::R32_UINT;
    uintBufferDesc.canHaveTypedViews = true;
    uintBufferDesc.canHaveUAVs = true;
    uintBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    uintBufferDesc.keepInitialState = true;
    uintBufferDesc.debugName = "GaussianSplatSortedIndexBuffer";
    m_indexBuffer = m_device->createBuffer(uintBufferDesc);

    uintBufferDesc.debugName = "GaussianSplatSortKeyBuffer";
    m_sortKeyBuffer = m_device->createBuffer(uintBufferDesc);

    nvrhi::BufferDesc sortControlDesc;
    sortControlDesc.byteSize = sizeof(uint32_t);
    sortControlDesc.format = nvrhi::Format::R32_UINT;
    sortControlDesc.canHaveTypedViews = true;
    sortControlDesc.canHaveUAVs = true;
    sortControlDesc.debugName = "GaussianSplatSortControlBuffer";
    sortControlDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    sortControlDesc.keepInitialState = true;
    m_sortControlBuffer = m_device->createBuffer(sortControlDesc);

    nvrhi::BufferDesc drawIndirectDesc;
    drawIndirectDesc.byteSize = sizeof(nvrhi::DrawIndirectArguments);
    drawIndirectDesc.format = nvrhi::Format::R32_UINT;
    drawIndirectDesc.canHaveTypedViews = true;
    drawIndirectDesc.canHaveUAVs = true;
    drawIndirectDesc.isDrawIndirectArgs = true;
    drawIndirectDesc.debugName = "GaussianSplatDrawIndirectBuffer";
    drawIndirectDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    drawIndirectDesc.keepInitialState = true;
    m_drawIndirectBuffer = m_device->createBuffer(drawIndirectDesc);

    nvrhi::BufferDesc aabbBufferDesc;
    aabbBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(nvrhi::rt::GeometryAABB);
    aabbBufferDesc.debugName = "GaussianSplatAabbBuffer";
    aabbBufferDesc.isAccelStructBuildInput = true;
    aabbBufferDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
    aabbBufferDesc.keepInitialState = true;
    m_splatAabbBuffer = m_device->createBuffer(aabbBufferDesc);

    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_sortKeyBindingSet = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
    m_accelBuilder.release();
    m_sourceFileName = fileName.string();
    m_splatUploadPending = true;
    m_formatUploadPending = true;
    m_cachedEmissionProxyMaxCount = 0;
    m_cachedEmissionProxySplatScale = 1.0f;
    m_cachedEmissionProxyKernelDegree = 0;
    m_cachedEmissionProxyAdaptiveClamp = true;
    m_cachedEmissionProxyTintColor = float3(1.0f);
    m_cachedEmissionProxyAlphaCullThreshold = 0.0f;
    m_emissionProxyBuildPending = true;
    m_accelBuilder.release();
    m_sorter.onSplatCountChanged(m_splatCount);

    return true;
}

void GaussianSplatPass::buildEmissionProxies(
    uint32_t maxProxyCount,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp,
    float3 tintColor,
    float alphaCullThreshold)
{
    kernelDegree = std::min(kernelDegree, 5u);
    tintColor = float3(
        std::max(tintColor.x, 0.0f),
        std::max(tintColor.y, 0.0f),
        std::max(tintColor.z, 0.0f));
    alphaCullThreshold = std::max(alphaCullThreshold, 0.0f);

    if (!hasSplats() || maxProxyCount == 0)
    {
        m_emissionProxies.clear();
        m_cachedEmissionProxyMaxCount = maxProxyCount;
        m_cachedEmissionProxySplatScale = splatScale;
        m_cachedEmissionProxyKernelDegree = kernelDegree;
        m_cachedEmissionProxyAdaptiveClamp = adaptiveClamp;
        m_cachedEmissionProxyTintColor = tintColor;
        m_cachedEmissionProxyAlphaCullThreshold = alphaCullThreshold;
        m_emissionProxyBuildPending = false;
        return;
    }

    const bool tintChanged =
        std::abs(m_cachedEmissionProxyTintColor.x - tintColor.x) >= 1e-4f ||
        std::abs(m_cachedEmissionProxyTintColor.y - tintColor.y) >= 1e-4f ||
        std::abs(m_cachedEmissionProxyTintColor.z - tintColor.z) >= 1e-4f;

    if (!m_emissionProxyBuildPending
        && m_cachedEmissionProxyMaxCount == maxProxyCount
        && std::abs(m_cachedEmissionProxySplatScale - splatScale) < 1e-4f
        && m_cachedEmissionProxyKernelDegree == kernelDegree
        && m_cachedEmissionProxyAdaptiveClamp == adaptiveClamp
        && !tintChanged
        && std::abs(m_cachedEmissionProxyAlphaCullThreshold - alphaCullThreshold) < 1e-6f)
    {
        return;
    }

    std::vector<GaussianSplatEmissionProxy> candidates;
    candidates.reserve(m_splats.size());

    for (const caustica::GaussianSplatData& splat : m_splats)
    {
        const float opacity = std::max(splat.centerOpacity.w, 0.0f);
        if (opacity <= alphaCullThreshold)
            continue;

        const float3 extent = caustica::render::gaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp);
        const float radius = std::max(1e-4f, std::max(extent.x, std::max(extent.y, extent.z)));
        const float3 linearSh0 = caustica::render::srgbToLinear(float3(
            std::max(splat.color.x, 0.0f),
            std::max(splat.color.y, 0.0f),
            std::max(splat.color.z, 0.0f)) * tintColor);
        const float3 radiance = linearSh0 * opacity;
        const float weight = std::max(0.0f, caustica::render::luminance(radiance)) * radius * radius;
        if (weight <= 0.0f)
            continue;

        GaussianSplatEmissionProxy proxy;
        proxy.center = splat.centerOpacity.xyz();
        proxy.radius = radius;
        proxy.radiance = radiance;
        proxy.weight = weight;
        candidates.push_back(proxy);
    }

    if (candidates.size() > maxProxyCount)
    {
        auto byDescendingWeight = [](const GaussianSplatEmissionProxy& lhs, const GaussianSplatEmissionProxy& rhs)
        {
            return lhs.weight > rhs.weight;
        };

        std::nth_element(candidates.begin(), candidates.begin() + maxProxyCount, candidates.end(), byDescendingWeight);
        candidates.resize(maxProxyCount);
        std::sort(candidates.begin(), candidates.end(), byDescendingWeight);
    }

    m_emissionProxies = std::move(candidates);
    m_cachedEmissionProxyMaxCount = maxProxyCount;
    m_cachedEmissionProxySplatScale = splatScale;
    m_cachedEmissionProxyKernelDegree = kernelDegree;
    m_cachedEmissionProxyAdaptiveClamp = adaptiveClamp;
    m_cachedEmissionProxyTintColor = tintColor;
    m_cachedEmissionProxyAlphaCullThreshold = alphaCullThreshold;
    m_emissionProxyBuildPending = false;
}

void GaussianSplatPass::buildAccelerationStructures(
    nvrhi::ICommandList* commandList,
    bool useAABBs,
    bool useTLASInstances,
    bool allowBlasCompaction,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    if (!hasSplats() || !m_splatAabbBuffer)
        return;

    uploadSplatDataIfNeeded(commandList);

    caustica::render::GaussianSplatAccelBuildParams params;
    params.useAABBs = useAABBs;
    params.useTLASInstances = useTLASInstances;
    params.allowBlasCompaction = allowBlasCompaction;
    params.splatScale = splatScale;
    params.kernelDegree = kernelDegree;
    params.adaptiveClamp = adaptiveClamp;
    m_accelBuilder.build(commandList, params, m_splats, m_splatCount, m_splatAabbBuffer);
}

void GaussianSplatPass::releaseAccelerationStructures()
{
    m_accelBuilder.release(hasSplats());
}

void GaussianSplatPass::createBindingSets(const RenderTargets& renderTargets, nvrhi::rt::IAccelStruct* meshTopLevelAS)
{
    if (!m_splatBuffer || !m_colorBuffer || !m_shBuffer || !m_indexBuffer || !m_sortKeyBuffer || !m_sortControlBuffer || !m_drawIndirectBuffer)
        return;

    nvrhi::BindingSetDesc rasterRenderBindingSetDesc;
    rasterRenderBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_SRV(1, m_indexBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::RawBuffer_SRV(2, m_colorBuffer),
        nvrhi::BindingSetItem::RawBuffer_SRV(3, m_shBuffer),
        nvrhi::BindingSetItem::Texture_SRV(4, renderTargets.depth)
    };
    m_rasterRenderBindingSet = m_device->createBindingSet(rasterRenderBindingSetDesc, m_rasterRenderBindingLayout);

    if (meshTopLevelAS != nullptr)
    {
        nvrhi::BindingSetDesc hybridRenderBindingSetDesc = rasterRenderBindingSetDesc;
        hybridRenderBindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::RayTracingAccelStruct(5, meshTopLevelAS));
        m_hybridRenderBindingSet = m_device->createBindingSet(hybridRenderBindingSetDesc, m_hybridRenderBindingLayout);
        m_hybridRenderMeshTopLevelAS = meshTopLevelAS;
    }
    else
    {
        m_hybridRenderBindingSet = nullptr;
        m_hybridRenderMeshTopLevelAS = nullptr;
    }

    nvrhi::BindingSetDesc sortKeyBindingSetDesc;
    sortKeyBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_sortKeyBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(1, m_indexBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(2, m_sortControlBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(3, m_drawIndirectBuffer, nvrhi::Format::R32_UINT)
    };
    m_sortKeyBindingSet = m_device->createBindingSet(sortKeyBindingSetDesc, m_sortKeyBindingLayout);
}

void GaussianSplatPass::createStochasticFramebuffer(const RenderTargets& renderTargets)
{
    auto createFramebuffer = [this](
        const nvrhi::TextureHandle& colorTarget,
        nvrhi::TextureHandle& depthBuffer,
        std::shared_ptr<caustica::FramebufferFactory>& framebuffer,
        const char* depthName)
    {
        if (!colorTarget)
            return;

        const nvrhi::TextureDesc& colorDesc = colorTarget->getDesc();
        bool depthMatches = false;
        if (depthBuffer)
        {
            const nvrhi::TextureDesc& depthDesc = depthBuffer->getDesc();
            depthMatches = depthDesc.width == colorDesc.width
                && depthDesc.height == colorDesc.height
                && depthDesc.sampleCount == colorDesc.sampleCount
                && depthDesc.sampleQuality == colorDesc.sampleQuality;
        }

        if (!depthMatches)
        {
            const std::array<nvrhi::Format, 4> depthFormats = {
                nvrhi::Format::D32,
                nvrhi::Format::D24S8,
                nvrhi::Format::D32S8,
                nvrhi::Format::D16
            };
            const nvrhi::FormatSupport depthFeatures =
                nvrhi::FormatSupport::Texture |
                nvrhi::FormatSupport::DepthStencil;

            nvrhi::TextureDesc depthDesc;
            depthDesc.width = colorDesc.width;
            depthDesc.height = colorDesc.height;
            depthDesc.sampleCount = colorDesc.sampleCount;
            depthDesc.sampleQuality = colorDesc.sampleQuality;
            depthDesc.dimension = colorDesc.dimension;
            depthDesc.mipLevels = 1;
            depthDesc.format = nvrhi::utils::ChooseFormat(m_device, depthFeatures, depthFormats.data(), depthFormats.size());
            depthDesc.isTypeless = true;
            depthDesc.isRenderTarget = true;
            depthDesc.isUAV = false;
            depthDesc.useClearValue = true;
            depthDesc.clearValue = nvrhi::Color(0.0f);
            depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            depthDesc.keepInitialState = true;
            depthDesc.debugName = depthName;
            depthBuffer = m_device->createTexture(depthDesc);
        }

        const bool framebufferMatches = framebuffer
            && !framebuffer->renderTargets.empty()
            && framebuffer->renderTargets[0].Get() == colorTarget.Get()
            && framebuffer->depthTarget.Get() == depthBuffer.Get();
        if (!framebufferMatches)
        {
            framebuffer = std::make_shared<caustica::FramebufferFactory>(m_device);
            framebuffer->renderTargets = { colorTarget };
            framebuffer->depthTarget = depthBuffer;
        }
    };

    createFramebuffer(renderTargets.outputColor, m_stochasticDepthBuffer, m_stochasticFramebuffer, "GaussianSplatStochasticDepth");
    createFramebuffer(renderTargets.processedOutputColor, m_stochasticProcessedDepthBuffer, m_stochasticProcessedFramebuffer, "GaussianSplatStochasticProcessedDepth");
}

void GaussianSplatPass::createPipeline(const RenderTargets& renderTargets)
{
    if (!hasSplats())
        return;

    createStochasticFramebuffer(renderTargets);

    std::vector<caustica::ShaderMacro> rasterShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "0" })
    };
    m_rasterVertexShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "vs_main", &rasterShadowMacros, nvrhi::ShaderType::Vertex);
    m_rasterPixelShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "ps_main", &rasterShadowMacros, nvrhi::ShaderType::Pixel);

    std::vector<caustica::ShaderMacro> hybridShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "1" })
    };
    m_hybridVertexShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "vs_main", &hybridShadowMacros, nvrhi::ShaderType::Vertex);
    m_hybridPixelShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "ps_main", &hybridShadowMacros, nvrhi::ShaderType::Pixel);

    std::vector<caustica::ShaderMacro> sortKeyMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_SORT_KEYS", "1" })
    };
    m_sortKeyShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "cs_sort_keys", &sortKeyMacros, nvrhi::ShaderType::Compute);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
    pipelineDesc.VS = m_rasterVertexShader;
    pipelineDesc.PS = m_rasterPixelShader;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::BlendState::RenderTarget alphaBlend;
    alphaBlend.blendEnable = true;
    alphaBlend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    alphaBlend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    alphaBlend.srcBlendAlpha = nvrhi::BlendFactor::One;
    alphaBlend.destBlendAlpha = nvrhi::BlendFactor::One;
    pipelineDesc.renderState.blendState.targets[0] = alphaBlend;

    m_rasterRenderPipeline = m_device->createGraphicsPipeline(
        pipelineDesc,
        renderTargets.processedOutputFramebuffer->getFramebuffer(nvrhi::AllSubresources));

    pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
    pipelineDesc.VS = m_hybridVertexShader;
    pipelineDesc.PS = m_hybridPixelShader;
    m_hybridRenderPipeline = m_device->createGraphicsPipeline(
        pipelineDesc,
        renderTargets.processedOutputFramebuffer->getFramebuffer(nvrhi::AllSubresources));

    if (m_stochasticFramebuffer)
    {
        nvrhi::BlendState::RenderTarget opaqueBlend;
        opaqueBlend.blendEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = opaqueBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
        pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

        pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
        pipelineDesc.VS = m_rasterVertexShader;
        pipelineDesc.PS = m_rasterPixelShader;
        m_stochasticRasterRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticFramebuffer->getFramebuffer(nvrhi::AllSubresources));

        pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
        pipelineDesc.VS = m_hybridVertexShader;
        pipelineDesc.PS = m_hybridPixelShader;
        m_stochasticHybridRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticFramebuffer->getFramebuffer(nvrhi::AllSubresources));
    }

    if (m_stochasticProcessedFramebuffer)
    {
        nvrhi::BlendState::RenderTarget opaqueBlend;
        opaqueBlend.blendEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = opaqueBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
        pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

        pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
        pipelineDesc.VS = m_rasterVertexShader;
        pipelineDesc.PS = m_rasterPixelShader;
        m_stochasticProcessedRasterRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticProcessedFramebuffer->getFramebuffer(nvrhi::AllSubresources));

        pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
        pipelineDesc.VS = m_hybridVertexShader;
        pipelineDesc.PS = m_hybridPixelShader;
        m_stochasticProcessedHybridRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticProcessedFramebuffer->getFramebuffer(nvrhi::AllSubresources));
    }

    nvrhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.bindingLayouts = { m_sortKeyBindingLayout };
    computePipelineDesc.CS = m_sortKeyShader;
    m_sortKeyPipeline = m_device->createComputePipeline(computePipelineDesc);

    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
}

void GaussianSplatPass::uploadSplatDataIfNeeded(nvrhi::ICommandList* commandList)
{
    if (!m_splatUploadPending || m_splats.empty())
        return;

    commandList->writeBuffer(m_splatBuffer, m_splats.data(), m_splats.size() * sizeof(caustica::GaussianSplatData));
    m_splatUploadPending = false;
}

void GaussianSplatPass::uploadFormatDataIfNeeded(
    nvrhi::ICommandList* commandList,
    GaussianSplatStorageFormat shFormat,
    GaussianSplatStorageFormat rgbaFormat)
{
    if (!hasSplats())
        return;

    const bool formatChanged = !m_colorBuffer || !m_shBuffer || shFormat != m_currentShFormat || rgbaFormat != m_currentRgbaFormat;
    if (formatChanged)
    {
        m_currentShFormat = shFormat;
        m_currentRgbaFormat = rgbaFormat;
        m_formatUploadPending = true;

        const uint64_t colorByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * 4u * FormatElementSize(rgbaFormat));
        nvrhi::BufferDesc colorDesc;
        colorDesc.byteSize = colorByteSize;
        colorDesc.canHaveRawViews = true;
        colorDesc.debugName = "GaussianSplatRGBAFormatBuffer";
        colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        colorDesc.keepInitialState = true;
        m_colorBuffer = m_device->createBuffer(colorDesc);

        constexpr uint32_t kShScalarStride = 45;
        const uint64_t shByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * kShScalarStride * FormatElementSize(shFormat));
        nvrhi::BufferDesc shDesc;
        shDesc.byteSize = shByteSize;
        shDesc.canHaveRawViews = true;
        shDesc.debugName = "GaussianSplatSHFormatBuffer";
        shDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        shDesc.keepInitialState = true;
        m_shBuffer = m_device->createBuffer(shDesc);

        m_rasterRenderBindingSet = nullptr;
        m_hybridRenderBindingSet = nullptr;
        m_hybridRenderMeshTopLevelAS = nullptr;
    }

    if (!m_formatUploadPending)
        return;

    m_packedColorOpacity.assign(size_t(AlignRawBufferSize(uint64_t(m_splatCount) * 4u * FormatElementSize(rgbaFormat))), 0u);
    for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
    {
        const float4 color = splatIndex < m_colorOpacity.size()
            ? m_colorOpacity[splatIndex]
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
        const uint64_t base = uint64_t(splatIndex) * 4u;
        StoreFormattedScalar(m_packedColorOpacity, base + 0u, rgbaFormat, color.x, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 1u, rgbaFormat, color.y, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 2u, rgbaFormat, color.z, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 3u, rgbaFormat, color.w, false);
    }

    constexpr uint32_t kShScalarStride = 45;
    m_packedShCoefficients.assign(size_t(AlignRawBufferSize(uint64_t(m_splatCount) * kShScalarStride * FormatElementSize(shFormat))), 0u);
    for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
    {
        for (uint32_t scalarIndex = 0; scalarIndex < kShScalarStride; ++scalarIndex)
        {
            StoreFormattedScalar(
                m_packedShCoefficients,
                uint64_t(splatIndex) * kShScalarStride + scalarIndex,
                shFormat,
                ShCoefficientAt(m_shCoefficients, splatIndex, scalarIndex),
                true);
        }
    }

    commandList->writeBuffer(m_colorBuffer, m_packedColorOpacity.data(), m_packedColorOpacity.size());
    commandList->writeBuffer(m_shBuffer, m_packedShCoefficients.data(), m_packedShCoefficients.size());
    m_formatUploadPending = false;
}

void GaussianSplatPass::render(
    nvrhi::ICommandList* commandList,
    const caustica::IView& view,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    const RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    if (!settings.enabled || !hasSplats())
        return;

    if (settings.sortingMode == GaussianSplatSortMode::GpuSort && !m_gpuSort)
        return;

    const bool stochasticSplats = settings.sortingMode == GaussianSplatSortMode::StochasticSplats;
    const bool distanceStageCulling = settings.frustumCulling == GaussianSplatFrustumCulling::AtDistanceStage;
    const bool stochasticToOutput = stochasticSplats && settings.renderTarget == GaussianSplatRenderTarget::OutputColor;
    nvrhi::TextureHandle stochasticDepthBuffer = stochasticToOutput ? m_stochasticDepthBuffer : m_stochasticProcessedDepthBuffer;
    std::shared_ptr<caustica::FramebufferFactory> stochasticFramebuffer = stochasticToOutput
        ? m_stochasticFramebuffer
        : m_stochasticProcessedFramebuffer;
    if (stochasticSplats && (!stochasticFramebuffer || !stochasticDepthBuffer))
        return;
    if (!stochasticSplats && !m_rasterRenderPipeline)
        return;

    commandList->beginMarker("GaussianSplats");

    uploadSplatDataIfNeeded(commandList);
    uploadFormatDataIfNeeded(commandList, settings.shFormat, settings.rgbaFormat);

    const bool useHybridShadows = settings.shadowsEnabled && meshTopLevelAS != nullptr && m_hybridRenderPipeline;

    if (!m_rasterRenderBindingSet || (useHybridShadows && (!m_hybridRenderBindingSet || m_hybridRenderMeshTopLevelAS != meshTopLevelAS)))
        createBindingSets(renderTargets, useHybridShadows ? meshTopLevelAS : nullptr);

    nvrhi::BindingSetHandle renderBindingSet = useHybridShadows ? m_hybridRenderBindingSet : m_rasterRenderBindingSet;
    if (!renderBindingSet)
    {
        commandList->endMarker();
        return;
    }

    PlanarViewConstants planarView = {};
    view.fillPlanarViewConstants(planarView);

    GaussianSplatConstants constants = {};
    constants.view = FromPlanarViewConstants(planarView);
    const float3 cameraPosition = view.getViewOrigin();
    constants.cameraPosition = float4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    constants.objectToWorld = settings.objectToWorld;
    constants.splatScale = settings.splatScale;
    constants.alphaScale = settings.alphaScale;
    constants.brightness = settings.brightness;
    constants.splatCount = m_splatCount;
    constants.tintColor = float3(
        std::max(settings.tintColor.x, 0.0f),
        std::max(settings.tintColor.y, 0.0f),
        std::max(settings.tintColor.z, 0.0f));
    constants.alphaCullThreshold = settings.alphaCullThreshold;
    constants.shDegree = m_shDegree;
    constants.depthTest = settings.depthTest ? 1u : 0u;
    constants.shadowsEnabled = useHybridShadows ? 1u : 0u;
    float3 shadowDir = settings.shadowDirectionToLight;
    if (length(shadowDir) < 1e-4f)
        shadowDir = float3(0.0f, 1.0f, 0.0f);
    shadowDir = normalize(shadowDir);
    constants.shadowDirectionToLight = float4(shadowDir.x, shadowDir.y, shadowDir.z, settings.shadowRayOffset);
    constants.shadowStrength = settings.shadowStrength;
    constants.shadowRayTMax = settings.shadowRayTMax;
    constants.shadowMode = useHybridShadows ? settings.shadowMode : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    constants.shadowSoftSampleCount = std::clamp(settings.shadowSoftSampleCount, 1u, 16u);
    constants.shadowSoftRadius = settings.shadowSoftRadius;
    constants.shadowFrameIndex = settings.shadowFrameIndex;
    constants.sortMode = uint32_t(settings.sortingMode);
    constants.frustumCulling = uint32_t(settings.frustumCulling);
    constants.frustumDilation = settings.frustumDilation;
    constants.minPixelCoverage = settings.minPixelCoverage;
    constants.screenSizeCulling = settings.screenSizeCulling ? 1u : 0u;
    constants.mipSplattingAntialiasing = settings.mipSplattingAntialiasing ? 1u : 0u;
    constants.shFormat = uint32_t(settings.shFormat);
    constants.rgbaFormat = uint32_t(settings.rgbaFormat);
    constants.projectionMethod = uint32_t(settings.projectionMethod);
    constants.stochasticFrameIndex = settings.stochasticFrameIndex;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    m_sorter.updateIndices(commandList, constants, settings.sortingMode, makeSortResources());

    nvrhi::GraphicsPipelineHandle renderPipeline;
    if (useHybridShadows)
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput ? m_stochasticHybridRenderPipeline : m_stochasticProcessedHybridRenderPipeline)
            : m_hybridRenderPipeline;
    }
    else
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput ? m_stochasticRasterRenderPipeline : m_stochasticProcessedRasterRenderPipeline)
            : m_rasterRenderPipeline;
    }
    nvrhi::IFramebuffer* framebuffer = stochasticSplats
        ? stochasticFramebuffer->getFramebuffer(nvrhi::AllSubresources)
        : renderTargets.processedOutputFramebuffer->getFramebuffer(nvrhi::AllSubresources);
    if (!renderPipeline || !framebuffer)
    {
        commandList->endMarker();
        return;
    }

    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::ShaderResource);
    if (distanceStageCulling)
        commandList->setBufferState(m_drawIndirectBuffer, nvrhi::ResourceStates::IndirectArgument);
    commandList->setTextureState(renderTargets.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    nvrhi::TextureHandle colorTarget = stochasticToOutput ? renderTargets.outputColor : renderTargets.processedOutputColor;
    commandList->setTextureState(colorTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    if (stochasticSplats)
        commandList->setTextureState(stochasticDepthBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();

    if (stochasticSplats)
    {
        const nvrhi::FormatInfo& depthFormatInfo = nvrhi::getFormatInfo(stochasticDepthBuffer->getDesc().format);
        commandList->clearDepthStencilTexture(stochasticDepthBuffer, nvrhi::AllSubresources, true, 0.0f, depthFormatInfo.hasStencil, 0);
    }

    nvrhi::GraphicsState state;
    state.pipeline = renderPipeline;
    state.bindings = { renderBindingSet };
    state.framebuffer = framebuffer;
    state.viewport = caustica::toNvrhi(view.getViewportState());
    if (distanceStageCulling)
        state.indirectParams = m_drawIndirectBuffer;
    commandList->setGraphicsState(state);

    if (distanceStageCulling)
    {
        commandList->drawIndirect(0);
    }
    else
    {
        nvrhi::DrawArguments args;
        args.vertexCount = m_splatCount * 6;
        args.instanceCount = 1;
        commandList->draw(args);
    }

    commandList->endMarker();
}
