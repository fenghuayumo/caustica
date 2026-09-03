#include <render/passes/gaussian/GaussianSplatPass.h>
#include <render/passes/gaussian/GaussianSplatGeometry.h>
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
            const float16_t halfValue = float32ToFloat16(value);
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

    // Keep each write comfortably below the per-command-list upload heap cap.
    // A degree-3 SH payload is roughly 192 bytes per splat, so a single write
    // for a million-splat asset would otherwise exceed D3D12's 256 MiB upload
    // budget and poison the command list.
    constexpr uint64_t kGaussianUploadChunkBytes = 16ull * 1024ull * 1024ull;

    bool UploadBufferChunk(
        caustica::rhi::CommandList* commandList,
        caustica::rhi::Buffer* buffer,
        const void* data,
        uint64_t byteSize,
        uint64_t& byteOffset)
    {
        if (byteOffset >= byteSize)
            return true;
        if (commandList == nullptr || buffer == nullptr || data == nullptr)
            return false;

        const uint64_t chunkSize = std::min(kGaussianUploadChunkBytes, byteSize - byteOffset);
        commandList->writeBuffer(
            buffer,
            static_cast<const uint8_t*>(data) + byteOffset,
            size_t(chunkSize),
            byteOffset);
        byteOffset += chunkSize;
        return byteOffset == byteSize;
    }
}

GaussianSplatPass::GaussianSplatPass(
    caustica::rhi::Device* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
    , m_accelBuilder(device)
{
    m_constantBuffer = m_device->createBuffer(caustica::rhi::utils::CreateVolatileConstantBufferDesc(sizeof(GaussianSplatConstants), "GaussianSplatConstants", 16));

    caustica::rhi::BindingLayoutDesc rasterRenderLayoutDesc;
    rasterRenderLayoutDesc.visibility = caustica::rhi::ShaderType::Vertex | caustica::rhi::ShaderType::Pixel;
    rasterRenderLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(2),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(3),
        caustica::rhi::BindingLayoutItem::Texture_SRV(4)
    };
    m_rasterRenderBindingLayout = m_device->createBindingLayout(rasterRenderLayoutDesc);

    caustica::rhi::BindingLayoutDesc hybridRenderLayoutDesc = rasterRenderLayoutDesc;
    hybridRenderLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::RayTracingAccelStruct(5));
    m_hybridRenderBindingLayout = m_device->createBindingLayout(hybridRenderLayoutDesc);

    caustica::rhi::BindingLayoutDesc sortLayoutDesc;
    sortLayoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    sortLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(0),
        caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(1),
        caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(2),
        caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(3)
    };
    m_sortKeyBindingLayout = m_device->createBindingLayout(sortLayoutDesc);
}

void GaussianSplatPass::setGpuSort(std::shared_ptr<GPUSort> gpuSort)
{
    m_gpuSort = std::move(gpuSort);
}

void GaussianSplatPass::setRayTracingRadianceResourcesEnabled(bool enabled)
{
    if (!enabled)
    {
        m_rayTracingShBuffer = nullptr;
        m_rayTracingShUploadPending = false;
        m_rayTracingShUploadOffset = 0;
        return;
    }

    if (m_rayTracingShBuffer || m_shCoefficients.empty())
        return;

    caustica::rhi::BufferDesc desc;
    desc.byteSize = uint64_t(m_shCoefficients.size()) * sizeof(float4);
    desc.structStride = sizeof(float4);
    desc.debugName = "GaussianSplatRayTracingSHBuffer";
    desc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    m_rayTracingShBuffer = m_device->createBuffer(desc);
    m_rayTracingShUploadPending = m_rayTracingShBuffer != nullptr;
    m_rayTracingShUploadOffset = 0;
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
    {
        // Match vk_gaussian_splatting: the degree-0 color is display RGB and is clamped
        // before storage regardless of the selected GPU format. SH residuals remain signed.
        m_colorOpacity.push_back(float4(
            Clamp01(splat.color.x),
            Clamp01(splat.color.y),
            Clamp01(splat.color.z),
            Clamp01(splat.centerOpacity.w)));
    }

    m_localBounds = box3::empty();
    for (const caustica::GaussianSplatData& splat : m_splats)
        m_localBounds |= splat.centerOpacity.xyz();
    m_localBoundsValid = !m_localBounds.isempty();

    if (m_shCoefficients.empty())
        m_shCoefficients.push_back(float4(0.0f, 0.0f, 0.0f, 0.0f));

    caustica::rhi::BufferDesc splatBufferDesc;
    splatBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(caustica::GaussianSplatData);
    splatBufferDesc.structStride = sizeof(caustica::GaussianSplatData);
    splatBufferDesc.debugName = "GaussianSplatDataBuffer";
    splatBufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    splatBufferDesc.keepInitialState = true;
    m_splatBuffer = m_device->createBuffer(splatBufferDesc);

    // Secondary-ray Gaussian radiance is an optional Hybrid-RT feature. Keep
    // its large uncompressed SH copy lazy, but preserve the resource path even
    // though 3DGRT is no longer exposed as a primary rendering method.
    m_rayTracingShBuffer = nullptr;
    m_rayTracingShUploadPending = false;
    m_rayTracingShUploadOffset = 0;

    m_colorBuffer = nullptr;
    m_shBuffer = nullptr;

    caustica::rhi::BufferDesc uintBufferDesc;
    uintBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(uint32_t);
    uintBufferDesc.format = caustica::rhi::Format::R32_UINT;
    uintBufferDesc.canHaveTypedViews = true;
    uintBufferDesc.canHaveUAVs = true;
    uintBufferDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    uintBufferDesc.keepInitialState = true;
    uintBufferDesc.debugName = "GaussianSplatSortedIndexBuffer";
    m_indexBuffer = m_device->createBuffer(uintBufferDesc);

    uintBufferDesc.debugName = "GaussianSplatSortKeyBuffer";
    m_sortKeyBuffer = m_device->createBuffer(uintBufferDesc);

    caustica::rhi::BufferDesc sortControlDesc;
    sortControlDesc.byteSize = sizeof(uint32_t);
    sortControlDesc.format = caustica::rhi::Format::R32_UINT;
    sortControlDesc.canHaveTypedViews = true;
    sortControlDesc.canHaveUAVs = true;
    sortControlDesc.debugName = "GaussianSplatSortControlBuffer";
    sortControlDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    sortControlDesc.keepInitialState = true;
    m_sortControlBuffer = m_device->createBuffer(sortControlDesc);

    caustica::rhi::BufferDesc drawIndirectDesc;
    drawIndirectDesc.byteSize = sizeof(caustica::rhi::DrawIndirectArguments);
    drawIndirectDesc.format = caustica::rhi::Format::R32_UINT;
    drawIndirectDesc.canHaveTypedViews = true;
    drawIndirectDesc.canHaveUAVs = true;
    drawIndirectDesc.isDrawIndirectArgs = true;
    drawIndirectDesc.debugName = "GaussianSplatDrawIndirectBuffer";
    drawIndirectDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    drawIndirectDesc.keepInitialState = true;
    m_drawIndirectBuffer = m_device->createBuffer(drawIndirectDesc);

    caustica::rhi::BufferDesc aabbBufferDesc;
    aabbBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(caustica::rhi::rt::GeometryAABB);
    aabbBufferDesc.debugName = "GaussianSplatAabbBuffer";
    aabbBufferDesc.isAccelStructBuildInput = true;
    aabbBufferDesc.initialState = caustica::rhi::ResourceStates::AccelStructBuildInput;
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
    m_splatUploadOffset = 0;
    m_colorUploadOffset = 0;
    m_shUploadOffset = 0;
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
    caustica::rhi::CommandList* commandList,
    bool useAABBs,
    bool useTLASInstances,
    bool allowBlasCompaction,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    if (!hasSplats() || !m_splatAabbBuffer)
        return;

    (void)uploadSplatDataIfNeeded(commandList);
    if (m_splatUploadPending)
        return;

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

void GaussianSplatPass::createBindingSets(const RenderTargets& renderTargets, caustica::rhi::rt::AccelStruct* meshTopLevelAS)
{
    if (!m_splatBuffer || !m_colorBuffer || !m_shBuffer || !m_indexBuffer || !m_sortKeyBuffer || !m_sortControlBuffer || !m_drawIndirectBuffer)
        return;

    caustica::rhi::BindingSetDesc rasterRenderBindingSetDesc;
    rasterRenderBindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(1, m_indexBuffer, caustica::rhi::Format::R32_UINT),
        caustica::rhi::BindingSetItem::RawBuffer_SRV(2, m_colorBuffer),
        caustica::rhi::BindingSetItem::RawBuffer_SRV(3, m_shBuffer),
        caustica::rhi::BindingSetItem::Texture_SRV(4, renderTargets.depth)
    };
    m_rasterRenderBindingSet = m_device->createBindingSet(rasterRenderBindingSetDesc, m_rasterRenderBindingLayout);

    if (meshTopLevelAS != nullptr)
    {
        caustica::rhi::BindingSetDesc hybridRenderBindingSetDesc = rasterRenderBindingSetDesc;
        hybridRenderBindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::RayTracingAccelStruct(5, meshTopLevelAS));
        m_hybridRenderBindingSet = m_device->createBindingSet(hybridRenderBindingSetDesc, m_hybridRenderBindingLayout);
        m_hybridRenderMeshTopLevelAS = meshTopLevelAS;
    }
    else
    {
        m_hybridRenderBindingSet = nullptr;
        m_hybridRenderMeshTopLevelAS = nullptr;
    }

    caustica::rhi::BindingSetDesc sortKeyBindingSetDesc;
    sortKeyBindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(0, m_sortKeyBuffer, caustica::rhi::Format::R32_UINT),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(1, m_indexBuffer, caustica::rhi::Format::R32_UINT),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(2, m_sortControlBuffer, caustica::rhi::Format::R32_UINT),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(3, m_drawIndirectBuffer, caustica::rhi::Format::R32_UINT)
    };
    m_sortKeyBindingSet = m_device->createBindingSet(sortKeyBindingSetDesc, m_sortKeyBindingLayout);
}

void GaussianSplatPass::createStochasticFramebuffer(const RenderTargets& renderTargets)
{
    auto createFramebuffer = [this](
        const caustica::rhi::TextureHandle& colorTarget,
        caustica::rhi::TextureHandle& depthBuffer,
        std::shared_ptr<caustica::FramebufferFactory>& framebuffer,
        const char* depthName)
    {
        if (!colorTarget)
            return;

        const caustica::rhi::TextureDesc& colorDesc = colorTarget->getDesc();
        bool depthMatches = false;
        if (depthBuffer)
        {
            const caustica::rhi::TextureDesc& depthDesc = depthBuffer->getDesc();
            depthMatches = depthDesc.width == colorDesc.width
                && depthDesc.height == colorDesc.height
                && depthDesc.sampleCount == colorDesc.sampleCount
                && depthDesc.sampleQuality == colorDesc.sampleQuality;
        }

        if (!depthMatches)
        {
            const std::array<caustica::rhi::Format, 4> depthFormats = {
                caustica::rhi::Format::D32,
                caustica::rhi::Format::D24S8,
                caustica::rhi::Format::D32S8,
                caustica::rhi::Format::D16
            };
            const caustica::rhi::FormatSupport depthFeatures =
                caustica::rhi::FormatSupport::Texture |
                caustica::rhi::FormatSupport::DepthStencil;

            caustica::rhi::TextureDesc depthDesc;
            depthDesc.width = colorDesc.width;
            depthDesc.height = colorDesc.height;
            depthDesc.sampleCount = colorDesc.sampleCount;
            depthDesc.sampleQuality = colorDesc.sampleQuality;
            depthDesc.dimension = colorDesc.dimension;
            depthDesc.mipLevels = 1;
            depthDesc.format = caustica::rhi::utils::ChooseFormat(m_device, depthFeatures, depthFormats.data(), depthFormats.size());
            depthDesc.isTypeless = true;
            depthDesc.isRenderTarget = true;
            depthDesc.isUAV = false;
            depthDesc.useClearValue = true;
            depthDesc.clearValue = caustica::rhi::Color(0.0f);
            depthDesc.initialState = caustica::rhi::ResourceStates::DepthWrite;
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
    createFramebuffer(renderTargets.ldrColor, m_stochasticLdrDepthBuffer, m_stochasticLdrFramebuffer, "GaussianSplatStochasticLdrDepth");
}

void GaussianSplatPass::createPipeline(const RenderTargets& renderTargets)
{
    if (!hasSplats())
        return;

    createStochasticFramebuffer(renderTargets);

    std::vector<caustica::ShaderMacro> rasterShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "0" })
    };
    auto createVsPs = [this](const char* path, std::vector<caustica::ShaderMacro>& macros,
        caustica::rhi::ShaderHandle& vs, caustica::rhi::ShaderHandle& ps)
    {
        vs = m_shaderFactory->createShader(path, "vs_main", &macros, caustica::rhi::ShaderType::Vertex);
        ps = m_shaderFactory->createShader(path, "ps_main", &macros, caustica::rhi::ShaderType::Pixel);
    };

    createVsPs("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", rasterShadowMacros,
        m_rasterVertexShader, m_rasterPixelShader);

    std::vector<caustica::ShaderMacro> hybridShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "1" })
    };
    createVsPs("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", hybridShadowMacros,
        m_hybridVertexShader, m_hybridPixelShader);
    createVsPs("caustica/shaders/render/processingPasses/GaussianSplatGutRaster.hlsl", rasterShadowMacros,
        m_gutRasterVertexShader, m_gutRasterPixelShader);
    createVsPs("caustica/shaders/render/processingPasses/GaussianSplatGutRaster.hlsl", hybridShadowMacros,
        m_gutHybridVertexShader, m_gutHybridPixelShader);

    std::vector<caustica::ShaderMacro> sortKeyMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_SORT_KEYS", "1" })
    };
    m_sortKeyShader = m_shaderFactory->createShader("caustica/shaders/render/processingPasses/GaussianSplatRaster.hlsl", "cs_sort_keys", &sortKeyMacros, caustica::rhi::ShaderType::Compute);

    caustica::rhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = caustica::rhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState.cullMode = caustica::rhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    caustica::rhi::BlendState::RenderTarget alphaBlend;
    alphaBlend.blendEnable = true;
    alphaBlend.srcBlend = caustica::rhi::BlendFactor::SrcAlpha;
    alphaBlend.destBlend = caustica::rhi::BlendFactor::InvSrcAlpha;
    alphaBlend.srcBlendAlpha = caustica::rhi::BlendFactor::One;
    alphaBlend.destBlendAlpha = caustica::rhi::BlendFactor::One;
    pipelineDesc.renderState.blendState.targets[0] = alphaBlend;

    auto createAlphaPipeline = [&](std::shared_ptr<caustica::FramebufferFactory> framebuffer,
        caustica::rhi::BindingLayoutHandle layout,
        caustica::rhi::ShaderHandle vs, caustica::rhi::ShaderHandle ps)
    {
        pipelineDesc.bindingLayouts = { layout };
        pipelineDesc.VS = vs;
        pipelineDesc.PS = ps;
        pipelineDesc.renderState.blendState.targets[0] = alphaBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
        return m_device->createGraphicsPipeline(
            pipelineDesc,
            framebuffer->getFramebuffer(caustica::rhi::AllSubresources)->getFramebufferInfo());
    };

    m_rasterRenderPipeline = createAlphaPipeline(renderTargets.processedOutputFramebuffer, m_rasterRenderBindingLayout, m_rasterVertexShader, m_rasterPixelShader);
    m_hybridRenderPipeline = createAlphaPipeline(renderTargets.processedOutputFramebuffer, m_hybridRenderBindingLayout, m_hybridVertexShader, m_hybridPixelShader);
    m_gutRasterRenderPipeline = createAlphaPipeline(renderTargets.processedOutputFramebuffer, m_rasterRenderBindingLayout, m_gutRasterVertexShader, m_gutRasterPixelShader);
    m_gutHybridRenderPipeline = createAlphaPipeline(renderTargets.processedOutputFramebuffer, m_hybridRenderBindingLayout, m_gutHybridVertexShader, m_gutHybridPixelShader);
    m_ldrRasterRenderPipeline = createAlphaPipeline(renderTargets.ldrFramebuffer, m_rasterRenderBindingLayout, m_rasterVertexShader, m_rasterPixelShader);
    m_ldrHybridRenderPipeline = createAlphaPipeline(renderTargets.ldrFramebuffer, m_hybridRenderBindingLayout, m_hybridVertexShader, m_hybridPixelShader);
    m_gutLdrRasterRenderPipeline = createAlphaPipeline(renderTargets.ldrFramebuffer, m_rasterRenderBindingLayout, m_gutRasterVertexShader, m_gutRasterPixelShader);
    m_gutLdrHybridRenderPipeline = createAlphaPipeline(renderTargets.ldrFramebuffer, m_hybridRenderBindingLayout, m_gutHybridVertexShader, m_gutHybridPixelShader);

    auto createStochasticPipeline = [&](std::shared_ptr<caustica::FramebufferFactory> fb,
        caustica::rhi::BindingLayoutHandle layout,
        caustica::rhi::ShaderHandle vs, caustica::rhi::ShaderHandle ps)
    {
        caustica::rhi::BlendState::RenderTarget opaqueBlend;
        opaqueBlend.blendEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = opaqueBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
        pipelineDesc.renderState.depthStencilState.depthFunc = caustica::rhi::ComparisonFunc::GreaterOrEqual;
        pipelineDesc.bindingLayouts = { layout };
        pipelineDesc.VS = vs;
        pipelineDesc.PS = ps;
        return m_device->createGraphicsPipeline(
            pipelineDesc,
            fb->getFramebuffer(caustica::rhi::AllSubresources)->getFramebufferInfo());
    };

    if (m_stochasticFramebuffer)
    {
        m_stochasticRasterRenderPipeline = createStochasticPipeline(
            m_stochasticFramebuffer, m_rasterRenderBindingLayout, m_rasterVertexShader, m_rasterPixelShader);
        m_stochasticHybridRenderPipeline = createStochasticPipeline(
            m_stochasticFramebuffer, m_hybridRenderBindingLayout, m_hybridVertexShader, m_hybridPixelShader);
        m_gutStochasticRasterRenderPipeline = createStochasticPipeline(
            m_stochasticFramebuffer, m_rasterRenderBindingLayout, m_gutRasterVertexShader, m_gutRasterPixelShader);
        m_gutStochasticHybridRenderPipeline = createStochasticPipeline(
            m_stochasticFramebuffer, m_hybridRenderBindingLayout, m_gutHybridVertexShader, m_gutHybridPixelShader);
    }

    if (m_stochasticProcessedFramebuffer)
    {
        m_stochasticProcessedRasterRenderPipeline = createStochasticPipeline(
            m_stochasticProcessedFramebuffer, m_rasterRenderBindingLayout, m_rasterVertexShader, m_rasterPixelShader);
        m_stochasticProcessedHybridRenderPipeline = createStochasticPipeline(
            m_stochasticProcessedFramebuffer, m_hybridRenderBindingLayout, m_hybridVertexShader, m_hybridPixelShader);
        m_gutStochasticProcessedRasterRenderPipeline = createStochasticPipeline(
            m_stochasticProcessedFramebuffer, m_rasterRenderBindingLayout, m_gutRasterVertexShader, m_gutRasterPixelShader);
        m_gutStochasticProcessedHybridRenderPipeline = createStochasticPipeline(
            m_stochasticProcessedFramebuffer, m_hybridRenderBindingLayout, m_gutHybridVertexShader, m_gutHybridPixelShader);
    }

    if (m_stochasticLdrFramebuffer)
    {
        m_stochasticLdrRasterRenderPipeline = createStochasticPipeline(
            m_stochasticLdrFramebuffer, m_rasterRenderBindingLayout, m_rasterVertexShader, m_rasterPixelShader);
        m_stochasticLdrHybridRenderPipeline = createStochasticPipeline(
            m_stochasticLdrFramebuffer, m_hybridRenderBindingLayout, m_hybridVertexShader, m_hybridPixelShader);
        m_gutStochasticLdrRasterRenderPipeline = createStochasticPipeline(
            m_stochasticLdrFramebuffer, m_rasterRenderBindingLayout, m_gutRasterVertexShader, m_gutRasterPixelShader);
        m_gutStochasticLdrHybridRenderPipeline = createStochasticPipeline(
            m_stochasticLdrFramebuffer, m_hybridRenderBindingLayout, m_gutHybridVertexShader, m_gutHybridPixelShader);
    }

    caustica::rhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.bindingLayouts = { m_sortKeyBindingLayout };
    computePipelineDesc.CS = m_sortKeyShader;
    m_sortKeyPipeline = m_device->createComputePipeline(computePipelineDesc);

    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
}

bool GaussianSplatPass::uploadSplatDataIfNeeded(caustica::rhi::CommandList* commandList)
{
    if (m_splats.empty())
        return true;

    if (m_splatUploadPending)
    {
        m_splatUploadPending = !UploadBufferChunk(
            commandList,
            m_splatBuffer,
            m_splats.data(),
            uint64_t(m_splats.size()) * sizeof(caustica::GaussianSplatData),
            m_splatUploadOffset);
    }

    if (m_rayTracingShUploadPending && m_rayTracingShBuffer && !m_shCoefficients.empty())
    {
        m_rayTracingShUploadPending = !UploadBufferChunk(
            commandList,
            m_rayTracingShBuffer,
            m_shCoefficients.data(),
            uint64_t(m_shCoefficients.size()) * sizeof(float4),
            m_rayTracingShUploadOffset);
    }

    return !m_splatUploadPending && !m_rayTracingShUploadPending;
}

void GaussianSplatPass::ensureFormatBuffers(
    GaussianSplatStorageFormat shFormat,
    GaussianSplatStorageFormat rgbaFormat)
{
    if (!hasSplats())
        return;

    const bool formatChanged = !m_colorBuffer || !m_shBuffer
        || shFormat != m_currentShFormat || rgbaFormat != m_currentRgbaFormat;
    if (!formatChanged)
        return;

    m_currentShFormat = shFormat;
    m_currentRgbaFormat = rgbaFormat;
    m_formatUploadPending = true;
    m_colorUploadOffset = 0;
    m_shUploadOffset = 0;

    const uint64_t colorByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * 4u * FormatElementSize(rgbaFormat));
    caustica::rhi::BufferDesc colorDesc;
    colorDesc.byteSize = colorByteSize;
    colorDesc.canHaveRawViews = true;
    colorDesc.debugName = "GaussianSplatRGBAFormatBuffer";
    colorDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    colorDesc.keepInitialState = true;
    m_colorBuffer = m_device->createBuffer(colorDesc);

    constexpr uint32_t kShScalarStride = 45;
    const uint64_t shByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * kShScalarStride * FormatElementSize(shFormat));
    caustica::rhi::BufferDesc shDesc;
    shDesc.byteSize = shByteSize;
    shDesc.canHaveRawViews = true;
    shDesc.debugName = "GaussianSplatSHFormatBuffer";
    shDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    shDesc.keepInitialState = true;
    m_shBuffer = m_device->createBuffer(shDesc);

    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
}

bool GaussianSplatPass::uploadFormatDataIfNeeded(
    caustica::rhi::CommandList* commandList,
    GaussianSplatStorageFormat shFormat,
    GaussianSplatStorageFormat rgbaFormat)
{
    if (!hasSplats())
        return true;

    ensureFormatBuffers(shFormat, rgbaFormat);

    if (!m_formatUploadPending)
        return true;

    if (m_colorUploadOffset == 0 && m_shUploadOffset == 0)
    {
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
    }

    const bool colorComplete = UploadBufferChunk(
        commandList, m_colorBuffer, m_packedColorOpacity.data(),
        m_packedColorOpacity.size(), m_colorUploadOffset);
    const bool shComplete = UploadBufferChunk(
        commandList, m_shBuffer, m_packedShCoefficients.data(),
        m_packedShCoefficients.size(), m_shUploadOffset);
    m_formatUploadPending = !(colorComplete && shComplete);
    return !m_formatUploadPending;
}

void GaussianSplatPass::prepareGraphResources(const GaussianSplatRenderSettings& settings)
{
    ensureFormatBuffers(settings.shFormat, settings.rgbaFormat);
}

GaussianSplatGraphResources GaussianSplatPass::graphResources(const GaussianSplatRenderSettings& settings) const
{
    const bool stochasticSplats = settings.sortingMode == GaussianSplatSortMode::StochasticSplats;
    const bool stochasticToOutput = stochasticSplats
        && settings.renderTarget == GaussianSplatRenderTarget::OutputColor;
    const bool stochasticToLdr = stochasticSplats
        && settings.renderTarget == GaussianSplatRenderTarget::LdrColor;

    return GaussianSplatGraphResources{
        .constantBuffer = m_constantBuffer.Get(),
        .splatBuffer = m_splatBuffer.Get(),
        .colorBuffer = m_colorBuffer.Get(),
        .shBuffer = m_shBuffer.Get(),
        .indexBuffer = m_indexBuffer.Get(),
        .sortKeyBuffer = m_sortKeyBuffer.Get(),
        .sortControlBuffer = m_sortControlBuffer.Get(),
        .drawIndirectBuffer = m_drawIndirectBuffer.Get(),
        .splatAabbBuffer = m_splatAabbBuffer.Get(),
        .stochasticDepth = stochasticSplats
            ? (stochasticToOutput
                ? m_stochasticDepthBuffer.Get()
                : (stochasticToLdr ? m_stochasticLdrDepthBuffer.Get() : m_stochasticProcessedDepthBuffer.Get()))
            : nullptr,
        .topLevelAS = m_accelBuilder.getTopLevelAS(),
        .sortMode = settings.sortingMode,
        .distanceStageCulling =
            settings.frustumCulling == GaussianSplatFrustumCulling::AtDistanceStage,
    };
}

bool GaussianSplatPass::upload(
    caustica::rhi::CommandList* commandList,
    const caustica::IView& view,
    caustica::rhi::rt::AccelStruct* meshTopLevelAS,
    const RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    m_framePrepared = false;
    if (!settings.enabled || !hasSplats())
        return false;

    if (settings.sortingMode == GaussianSplatSortMode::GpuSort && !m_gpuSort)
        return false;

    const bool stochasticSplats = settings.sortingMode == GaussianSplatSortMode::StochasticSplats;
    const bool distanceStageCulling = settings.frustumCulling == GaussianSplatFrustumCulling::AtDistanceStage;
    const bool stochasticToOutput = stochasticSplats && settings.renderTarget == GaussianSplatRenderTarget::OutputColor;
    const bool renderToLdr = settings.renderTarget == GaussianSplatRenderTarget::LdrColor;
    const bool stochasticToLdr = stochasticSplats && renderToLdr;
    caustica::rhi::TextureHandle stochasticDepthBuffer = stochasticToOutput
        ? m_stochasticDepthBuffer
        : (stochasticToLdr ? m_stochasticLdrDepthBuffer : m_stochasticProcessedDepthBuffer);
    std::shared_ptr<caustica::FramebufferFactory> stochasticFramebuffer = stochasticToOutput
        ? m_stochasticFramebuffer
        : (stochasticToLdr ? m_stochasticLdrFramebuffer : m_stochasticProcessedFramebuffer);
    if (stochasticSplats && (!stochasticFramebuffer || !stochasticDepthBuffer))
        return false;
    if (!stochasticSplats)
    {
        const bool hasPrimaryPipeline = settings.primaryMethod == GaussianSplatPrimaryMethod::GUT
            ? m_gutRasterRenderPipeline != nullptr
            : m_rasterRenderPipeline != nullptr;
        if (!hasPrimaryPipeline)
            return false;
    }

    commandList->beginMarker("GaussianSplatsUpload");

    const bool splatDataReady = uploadSplatDataIfNeeded(commandList);
    const bool formatDataReady = uploadFormatDataIfNeeded(
        commandList, settings.shFormat, settings.rgbaFormat);
    if (!splatDataReady || !formatDataReady)
    {
        commandList->endMarker();
        return false;
    }

    const bool useHybridShadows = settings.shadowsEnabled
        && settings.shadowLightCount > 0
        && meshTopLevelAS != nullptr
        && m_hybridRenderPipeline;

    if (!m_rasterRenderBindingSet || (useHybridShadows && (!m_hybridRenderBindingSet || m_hybridRenderMeshTopLevelAS != meshTopLevelAS)))
        createBindingSets(renderTargets, useHybridShadows ? meshTopLevelAS : nullptr);

    caustica::rhi::BindingSetHandle renderBindingSet = useHybridShadows ? m_hybridRenderBindingSet : m_rasterRenderBindingSet;
    if (!renderBindingSet)
    {
        commandList->endMarker();
        return false;
    }

    PlanarViewConstants planarView = {};
    view.fillPlanarViewConstants(planarView);

    GaussianSplatConstants constants = {};
    constants.view = FromPlanarViewConstants(planarView);
    const float3 cameraPosition = view.getViewOrigin();
    constants.cameraPosition = float4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    constants.objectToWorld = settings.objectToWorld;
    {
        // SH / 3DGUT kernel evaluation is in object/training space.
        const float4x4 worldToObject = inverse(settings.objectToWorld);
        constants.worldToObject = worldToObject;
        const float4 cameraObject = float4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f) * worldToObject;
        constants.cameraPositionObject = float4(cameraObject.x, cameraObject.y, cameraObject.z, 1.0f);
    }
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
    constants.depthBias = std::max(settings.depthBias, 0.0f);
    constants.depthEdgeDilation = settings.depthEdgeDilation ? 1u : 0u;
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
    constants.shadowLightCount = useHybridShadows
        ? std::min(settings.shadowLightCount, uint32_t(GAUSSIAN_SPLAT_MAX_RECEIVER_SHADOW_LIGHTS))
        : 0u;
    for (uint32_t lightIndex = 0; lightIndex < constants.shadowLightCount; ++lightIndex)
        constants.shadowLights[lightIndex] = settings.shadowLights[lightIndex];
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
    constants.covarianceDilation = std::max(settings.covarianceDilation, 0.0f);
    constants.referenceGammaCompositing = settings.referenceGammaCompositing ? 1u : 0u;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    const bool useGut = settings.primaryMethod == GaussianSplatPrimaryMethod::GUT;
    caustica::rhi::GraphicsPipelineHandle renderPipeline;
    if (useHybridShadows)
    {
        if (useGut)
        {
            renderPipeline = stochasticSplats
                ? (stochasticToOutput
                    ? m_gutStochasticHybridRenderPipeline
                    : (stochasticToLdr ? m_gutStochasticLdrHybridRenderPipeline : m_gutStochasticProcessedHybridRenderPipeline))
                : (renderToLdr ? m_gutLdrHybridRenderPipeline : m_gutHybridRenderPipeline);
        }
        else
        {
            renderPipeline = stochasticSplats
                ? (stochasticToOutput
                    ? m_stochasticHybridRenderPipeline
                    : (stochasticToLdr ? m_stochasticLdrHybridRenderPipeline : m_stochasticProcessedHybridRenderPipeline))
                : (renderToLdr ? m_ldrHybridRenderPipeline : m_hybridRenderPipeline);
        }
    }
    else if (useGut)
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput
                ? m_gutStochasticRasterRenderPipeline
                : (stochasticToLdr ? m_gutStochasticLdrRasterRenderPipeline : m_gutStochasticProcessedRasterRenderPipeline))
            : (renderToLdr ? m_gutLdrRasterRenderPipeline : m_gutRasterRenderPipeline);
    }
    else
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput
                ? m_stochasticRasterRenderPipeline
                : (stochasticToLdr ? m_stochasticLdrRasterRenderPipeline : m_stochasticProcessedRasterRenderPipeline))
            : (renderToLdr ? m_ldrRasterRenderPipeline : m_rasterRenderPipeline);
    }
    caustica::rhi::Framebuffer* framebuffer = stochasticSplats
        ? stochasticFramebuffer->getFramebuffer(caustica::rhi::AllSubresources)
        : (renderToLdr
            ? renderTargets.ldrFramebuffer->getFramebuffer(caustica::rhi::AllSubresources)
            : renderTargets.processedOutputFramebuffer->getFramebuffer(caustica::rhi::AllSubresources));
    if (!renderPipeline || !framebuffer)
    {
        commandList->endMarker();
        return false;
    }

    m_frameRenderSettings = settings;
    m_frameConstants = constants;
    m_frameRenderBindingSet = renderBindingSet;
    m_frameRenderPipeline = renderPipeline;
    m_frameFramebuffer = framebuffer;
    m_frameStochasticDepthBuffer = stochasticDepthBuffer;
    m_frameDistanceStageCulling = distanceStageCulling;
    m_framePrepared = true;

    commandList->endMarker();
    return true;
}

void GaussianSplatPass::sort(caustica::rhi::CommandList* commandList)
{
    if (!m_framePrepared)
        return;

    commandList->beginMarker("GaussianSplatsSort");
    // Volatile CBs are per command-list open session. Upload may have run on a
    // prior list instance (e.g. after FrameCommandContext::flushPrimary).
    commandList->writeBuffer(m_constantBuffer, &m_frameConstants, sizeof(m_frameConstants));
    m_sorter.updateIndices(
        commandList,
        m_frameConstants,
        m_frameRenderSettings.sortingMode,
        makeSortResources());
    commandList->endMarker();
}

bool GaussianSplatPass::raster(
    caustica::rhi::CommandList* commandList,
    const caustica::IView& view)
{
    if (!m_framePrepared)
        return false;

    commandList->beginMarker("GaussianSplatsRaster");
    commandList->writeBuffer(m_constantBuffer, &m_frameConstants, sizeof(m_frameConstants));

    const bool stochasticSplats =
        m_frameRenderSettings.sortingMode == GaussianSplatSortMode::StochasticSplats;
    if (stochasticSplats)
    {
        const caustica::rhi::FormatInfo& depthFormatInfo =
            caustica::rhi::getFormatInfo(m_frameStochasticDepthBuffer->getDesc().format);
        commandList->clearDepthStencilTexture(
            m_frameStochasticDepthBuffer,
            caustica::rhi::AllSubresources,
            true,
            0.0f,
            depthFormatInfo.hasStencil,
            0);
    }

    caustica::rhi::GraphicsState state;
    state.pipeline = m_frameRenderPipeline;
    state.bindings = { m_frameRenderBindingSet };
    state.framebuffer = m_frameFramebuffer;
    state.viewport = caustica::toRhi(view.getViewportState());
    if (m_frameDistanceStageCulling)
        state.indirectParams = m_drawIndirectBuffer;
    commandList->setGraphicsState(state);

    if (m_frameDistanceStageCulling)
    {
        commandList->drawIndirect(0);
    }
    else
    {
        caustica::rhi::DrawArguments args;
        args.vertexCount = m_splatCount * 6;
        args.instanceCount = 1;
        commandList->draw(args);
    }

    commandList->endMarker();
    m_framePrepared = false;
    return true;
}
