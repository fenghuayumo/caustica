#include <render/passes/rtxdi/PrepareLightsPass.h>
#include <render/SceneGpuResources.h>
#include <render/passes/rtxdi/RtxdiResources.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneObjects.h>

#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderDevice.h>
#include <core/log.h>
#include <rhi/utils.h>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace caustica::math;
#include <shaders/render/rtxdi/ShaderParameters.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/core/RenderTargets.h>

#include <render/passes/debug/ShaderDebug.h>

#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>

using namespace caustica;


PrepareLightsPass::PrepareLightsPass(
    caustica::rhi::IDevice* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory,
    caustica::render::RenderDevice& renderDevice,
    std::shared_ptr<MaterialGpuCache> materialGpuCache,
    std::shared_ptr<OpacityMicromapBuilder> opacityMicromapBuilder,
    caustica::rhi::BufferHandle subInstanceData,
    caustica::rhi::IBindingLayout* bindlessLayout,
    std::shared_ptr<ShaderDebug> shaderDebug)
    : m_device(device)
    , m_bindlessLayout(bindlessLayout)
    , m_shaderFactory(std::move(shaderFactory))
    , m_renderDevice(renderDevice)
    , m_materialGpuCache(materialGpuCache)
    , m_opacityMicromapBuilder(opacityMicromapBuilder)
    , m_subInstanceData(subInstanceData)
    , m_shaderDebug(shaderDebug)
{
    caustica::rhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    bindingLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0), //PushConstants(0, sizeof(PrepareLightsConstants)),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(0),
        caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(1),
        caustica::rhi::BindingLayoutItem::Texture_UAV(2),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        //caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        caustica::rhi::BindingLayoutItem::Texture_SRV(6),
        caustica::rhi::BindingLayoutItem::Texture_SRV(7),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(8),
        caustica::rhi::BindingLayoutItem::Texture_UAV(50),

        caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),

        caustica::rhi::BindingLayoutItem::Sampler(0),
        caustica::rhi::BindingLayoutItem::Sampler(1),
        caustica::rhi::BindingLayoutItem::Sampler(2)
    };

    m_bindingLayout = m_device->createBindingLayout(bindingLayoutDesc);

    caustica::rhi::BufferDesc constBufferDesc;
    constBufferDesc.byteSize = sizeof(PrepareLightsConstants);
    constBufferDesc.debugName = "PrepareLightsConstants";
    constBufferDesc.isConstantBuffer = true;
    constBufferDesc.isVolatile = true;
    constBufferDesc.maxVersions = 16;
    m_constantBuffer = device->createBuffer(constBufferDesc);
}


void PrepareLightsPass::setFrameInputs(
    const scene::SceneRenderData* renderData,
    size_t geometryInstanceCount,
    caustica::rhi::IDescriptorTable* descriptorTable,
    const caustica::render::SceneGpuFrameHandles& gpuHandles,
    EnvMapProcessor* environmentMap,
    EnvMapSceneParams envMapSceneParams)
{
    m_renderData = renderData;
    m_geometryInstanceCount = geometryInstanceCount;
    m_descriptorTable = descriptorTable;
    m_gpuHandles = gpuHandles;
    m_EnvironmentMap = environmentMap;
    m_EnvironmentMapSceneParams = envMapSceneParams;
}

void PrepareLightsPass::setGaussianSplatEmissionProxies(
    const std::vector<GaussianSplatEmissionProxy>* proxies,
    float4x4 objectToWorld,
    float emissionIntensity)
{
    m_GaussianSplatEmissionProxies = proxies;
    m_GaussianSplatEmissionObjectToWorld = objectToWorld;
    m_GaussianSplatEmissionIntensity = emissionIntensity;
}

void PrepareLightsPass::createPipeline()
{
    caustica::debug("Initializing PrepareLightsPass...");

    m_computeShader = m_shaderFactory->createShader("caustica/shaders/render/rtxdi/prepareLights.hlsl", "main", nullptr, caustica::rhi::ShaderType::Compute);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
    pipelineDesc.CS = m_computeShader;
    m_computePipeline = m_device->createComputePipeline(pipelineDesc);
}

void PrepareLightsPass::createBindingSet(RtxdiResources& resources, const RenderTargets& renderTargets)
{
    if (!m_gpuHandles.valid())
        return;

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),// PushConstants(0, sizeof(PrepareLightsConstants)),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(0, resources.LightDataBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(1, resources.LightIndexMappingBuffer),
        caustica::rhi::BindingSetItem::Texture_UAV(2, resources.LocalLightPdfTexture),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(0, resources.TaskBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(1, m_subInstanceData),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(2, m_gpuHandles.instanceBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(3, m_gpuHandles.geometryBuffer),
        //caustica::rhi::BindingSetItem::StructuredBuffer_SRV(4, (m_opacityMicromapBuilder!=nullptr)?(m_opacityMicromapBuilder->getGeometryDebugBuffer()):(resources.LightDataBuffer.Get())), // yuck
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(5, m_materialGpuCache->getMaterialDataBuffer()),
        caustica::rhi::BindingSetItem::Texture_SRV(6, m_EnvironmentMap ? m_EnvironmentMap->getEnvMapCube() : m_renderDevice.builtins().blackCubeMapArray()),
        caustica::rhi::BindingSetItem::Texture_SRV(7, m_EnvironmentMap ? m_EnvironmentMap->getImportanceSampling()->getImportanceMapOnly() : m_renderDevice.builtins().blackTexture()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(8, resources.PrimitiveLightBuffer),
        caustica::rhi::BindingSetItem::Texture_UAV(50, m_shaderDebug->getDebugVizTexture()), // TODO: move to shader debug uav

        caustica::rhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),

        caustica::rhi::BindingSetItem::Sampler(0, m_renderDevice.samplers().anisotropicWrap()),
        caustica::rhi::BindingSetItem::Sampler(1, m_EnvironmentMap ? m_EnvironmentMap->getEnvMapCubeSampler() : m_renderDevice.samplers().anisotropicWrap()),
        caustica::rhi::BindingSetItem::Sampler(2, m_EnvironmentMap ? m_EnvironmentMap->getImportanceSampling()->getImportanceMapSampler() : m_renderDevice.samplers().anisotropicWrap())
    };

    m_bindingSet = m_device->createBindingSet(bindingSetDesc, m_bindingLayout);
    m_TaskBuffer = resources.TaskBuffer;
    m_PrimitiveLightBuffer = resources.PrimitiveLightBuffer;
    m_LightIndexMappingBuffer = resources.LightIndexMappingBuffer;
    m_GeometryInstanceToLightBuffer = resources.GeometryInstanceToLightBuffer;
    m_LocalLightPdfTexture = resources.LocalLightPdfTexture;
    m_MaxLightsInBuffer = uint32_t(resources.LightDataBuffer->getDesc().byteSize / (sizeof(PolymorphicLightInfoFull) * 2));
}

void PrepareLightsPass::countLightsInScene(uint32_t& numEmissiveMeshes, uint32_t& numEmissiveTriangles)
{
    numEmissiveMeshes = 0;
    numEmissiveTriangles = 0;
    if (!m_renderData)
        return;

    for (const scene::MeshInstanceRenderProxy& meshProxy : m_renderData->meshInstances)
    {
        const auto* mesh = m_renderData->findMesh(meshProxy.meshId);
        if (!mesh)
            continue;

        for (const auto& geometry : mesh->geometries)
        {
            std::shared_ptr<StandardMaterial> standardMaterial =
                m_materialGpuCache->findByResourceId(geometry.materialId);
            if (standardMaterial && standardMaterial->isEmissive())
            {
                numEmissiveMeshes += 1;
                numEmissiveTriangles += geometry.numIndices / 3;
            }
        }
    }
}

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

static void packLightColor(const float3& color, PolymorphicLightInfoFull& lightInfo)
{
    float maxRadiance = std::max(color.x, std::max(color.y, color.z));

    if (maxRadiance <= 0.f)
        return;

    float logRadiance = (::log2f(maxRadiance) - kPolymorphicLightMinLog2Radiance) / (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance);
    logRadiance = saturate(logRadiance);
    uint32_t packedRadiance = std::min(uint32_t(ceilf(logRadiance * 65534.f)) + 1, 0xffffu);
    float unpackedRadiance = ::exp2f((float(packedRadiance - 1) / 65534.f) * (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) + kPolymorphicLightMinLog2Radiance);

    lightInfo.Base.ColorTypeAndFlags |= FLOAT3_to_R8G8B8_UNORM(color.x / unpackedRadiance, color.y / unpackedRadiance, color.z / unpackedRadiance);
    lightInfo.Base.LogRadiance |= packedRadiance;
}

static float2 OctWrap(float2 v)
{
    return float2( ( 1.0f - abs(v.y)) * ((v.x >= 0.0)?1.0f:-1.0f), 
                   ( 1.0f - abs(v.x)) * ((v.y >= 0.0)?1.0f:-1.0f) );
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

static bool ConvertLightProxy(
    const caustica::scene::LightRenderProxy& proxy,
    PolymorphicLightInfoFull& polymorphic,
    bool enableImportanceSampledEnvironmentLight,
    EnvMapProcessor* /*environmentMap*/)
{
    using namespace caustica::scene;
    switch (getLightType(proxy))
    {
    case LightType_Spot: {
        const auto& spot = std::get<SpotLightData>(proxy.data);
        const float3 lightPos = float3(getLightPosition(proxy.transform));
        const float3 lightDir = float3(normalize(getLightDirection(proxy.transform)));

        if (spot.radius == 0.f)
        {
            float3 flux = proxy.color * spot.intensity;
            polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kPoint << kPolymorphicLightTypeShift | ((spot.outerAngle < 0) ? kPolymorphicLightShapingUseMinFalloff : 0);
            packLightColor(flux, polymorphic);
            polymorphic.Base.Center = lightPos;
            polymorphic.Base.Direction1 = NDirToOctUnorm32(lightDir);
            polymorphic.Base.Direction2 = fp32ToFp16(dm::radians(abs(spot.outerAngle)));
            polymorphic.Base.Direction2 |= fp32ToFp16(dm::radians(spot.innerAngle)) << 16;
        }
        else
        {
            float projectedArea = dm::PI_f * square(spot.radius);
            float3 radiance = proxy.color * spot.intensity / projectedArea;
            float softness = saturate(1.f - spot.innerAngle / abs(spot.outerAngle));
            polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift | ((spot.outerAngle < 0) ? kPolymorphicLightShapingUseMinFalloff : 0);
            polymorphic.Base.ColorTypeAndFlags |= kPolymorphicLightShapingEnableBit;
            packLightColor(radiance, polymorphic);
            polymorphic.Base.Center = lightPos;
            polymorphic.Base.Scalars = fp32ToFp16(spot.radius);
            polymorphic.Extended.PrimaryAxis = NDirToOctUnorm32(lightDir);
            polymorphic.Extended.CosConeAngleAndSoftness = fp32ToFp16(cosf(dm::radians(abs(spot.outerAngle))));
            polymorphic.Extended.CosConeAngleAndSoftness |= fp32ToFp16(softness) << 16;
        }
        return true;
    }
    case LightType_Point: {
        const auto& point = std::get<PointLightData>(proxy.data);
        const float3 lightPos = float3(getLightPosition(proxy.transform));

        if (point.radius == 0.f)
        {
            float3 flux = proxy.color * point.intensity;
            polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kPoint << kPolymorphicLightTypeShift;
            packLightColor(flux, polymorphic);
            polymorphic.Base.Center = lightPos;
            polymorphic.Base.Direction2 = fp32ToFp16(dm::PI_f) | fp32ToFp16(0.0f) << 16;
        }
        else
        {
            float projectedArea = dm::PI_f * square(point.radius);
            float3 radiance = proxy.color * point.intensity / projectedArea;
            polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift;
            packLightColor(radiance, polymorphic);
            polymorphic.Base.Center = lightPos;
            polymorphic.Base.Scalars = fp32ToFp16(point.radius);
        }

        return true;
    }
    case LightType_Environment: {
        if (!enableImportanceSampledEnvironmentLight)
            return false;
        polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kEnvironment << kPolymorphicLightTypeShift;
        polymorphic.Base.Direction2 = 0;
        return true;
    }
    default:
        return false;
    }
}

static int lightInfinityRank(const caustica::scene::LightRenderProxy& proxy)
{
    switch (caustica::scene::getLightType(proxy))
    {
    case LightType_Directional: return 1;
    case LightType_Environment: return 2;
    default: return 0;
    }
}

static uint32_t GaussianProxyHash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x21f0aaad;
    x ^= x >> 15;
    x *= 0xf35a2d97;
    x ^= x >> 15;
    return x;
}

static uint32_t GaussianProxyHash32Combine(uint32_t seed, uint32_t value)
{
    return seed ^ (GaussianProxyHash32(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
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
    PolymorphicLightInfoFull polymorphic = {};

    const float3 radiance = float3(
        std::max(proxy.radiance.x * emissionIntensity, 0.0f),
        std::max(proxy.radiance.y * emissionIntensity, 0.0f),
        std::max(proxy.radiance.z * emissionIntensity, 0.0f));
    const float radius = std::max(proxy.radius * TransformRadiusScale(objectToWorld), 1e-4f);

    polymorphic.Base.ColorTypeAndFlags = (uint32_t)PolymorphicLightType::kSphere << kPolymorphicLightTypeShift;
    packLightColor(radiance, polymorphic);
    polymorphic.Base.Center = TransformPoint(proxy.center, objectToWorld);
    polymorphic.Base.Scalars = fp32ToFp16(radius);
    polymorphic.Extended = PolymorphicLightInfoEx::empty();
    polymorphic.Extended.UniqueID = GaussianProxyHash32Combine(0x3D650000u, proxyIndex);

    return polymorphic;
}


RTXDI_LightBufferParameters PrepareLightsPass::process(caustica::rhi::ICommandList* commandList)
{
    RTXDI_LightBufferParameters lightBufferParams = {};

    if (!m_renderData || !m_descriptorTable)
        return lightBufferParams;

    commandList->beginMarker("prepareLights");

    std::vector<PrepareLightsTask> tasks;
    std::vector<PolymorphicLightInfoFull> primitiveLightInfos;
    uint32_t lightBufferOffset = 0;
    std::vector<uint32_t> geometryInstanceToLight(m_geometryInstanceCount, RTXDI_INVALID_LIGHT_INDEX);

    // Dense prefix must match AccelStructManager / MaterialGpuCache / PathTracingShaderCompiler.
    // Do not skip on stale proxy.geometryInstanceIndex after runtime import.
    const auto& renderData = *m_renderData;
    size_t compactedGeometryInstanceIndex = 0;
    for (const scene::MeshInstanceRenderProxy& meshProxy : renderData.meshInstances)
    {
        const auto* mesh = renderData.findMesh(meshProxy.meshId);
        if (!mesh)
            continue;

        const uint32_t firstGeometryInstanceIndex =
            static_cast<uint32_t>(compactedGeometryInstanceIndex);
        compactedGeometryInstanceIndex += mesh->geometries.size();

        for (size_t geometryIndex = 0; geometryIndex < mesh->geometries.size(); ++geometryIndex)
        {
            const auto& geometry = mesh->geometries[geometryIndex];
            const size_t geometryInstanceIndex = size_t(firstGeometryInstanceIndex) + geometryIndex;
            if (geometryInstanceIndex >= geometryInstanceToLight.size())
            {
                assert(false && "Geometry instance index is out of sync with scene geometry instances");
                continue;
            }

            size_t instanceHash = 0;
            caustica::rhi::hash_combine(instanceHash, static_cast<uint32_t>(meshProxy.entity));
            caustica::rhi::hash_combine(instanceHash, geometryIndex);

            std::shared_ptr<StandardMaterial> standardMaterialPtr =
                m_materialGpuCache->findByResourceId(geometry.materialId);
            if (!standardMaterialPtr)
                continue;
            StandardMaterial & standardMaterial = *standardMaterialPtr;
            if (!standardMaterial.isEmissive())
            {
                // remove the info about this instance, just in case it was emissive and now it's not
                m_InstanceLightBufferOffsets.erase(instanceHash);
                continue;
            }

            geometryInstanceToLight[geometryInstanceIndex] = lightBufferOffset;

            // find the previous offset of this instance in the light buffer
            auto pOffset = m_InstanceLightBufferOffsets.find(instanceHash);

            assert(geometryIndex < 0xfff);

            PrepareLightsTask task;
            task.instanceAndGeometryIndex = (meshProxy.instanceIndex << 12) | uint32_t(geometryIndex & 0xfff);
            task.lightBufferOffset = lightBufferOffset;
            task.triangleCount = geometry.numIndices / 3;
            task.previousLightBufferOffset = (pOffset != m_InstanceLightBufferOffsets.end()) ? int(pOffset->second) : -1;

            // record the current offset of this instance for use on the next frame
            m_InstanceLightBufferOffsets[instanceHash] = lightBufferOffset;

            lightBufferOffset += task.triangleCount;

            tasks.push_back(task);
        }
    }

    if (!geometryInstanceToLight.empty())
        commandList->writeBuffer(m_GeometryInstanceToLightBuffer, geometryInstanceToLight.data(), geometryInstanceToLight.size() * sizeof(uint32_t));

	lightBufferParams.localLightBufferRegion.firstLightIndex = 0;
	lightBufferParams.localLightBufferRegion.numLights = lightBufferOffset;

    // sort proxies: finite first (0), directional next (1), environment last (2)
    std::vector<const scene::LightRenderProxy*> sortedLights;
    sortedLights.reserve(renderData.lights.size());
    for (const scene::LightRenderProxy& light : renderData.lights)
        sortedLights.push_back(&light);
    std::sort(sortedLights.begin(), sortedLights.end(),
        [](const scene::LightRenderProxy* a, const scene::LightRenderProxy* b) {
            return lightInfinityRank(*a) < lightInfinityRank(*b);
        });

    uint32_t numFinitePrimLights = 0;
    uint32_t numInfinitePrimLights = 0;

    bool enableImportanceSampledEnvironmentLight = m_EnvironmentMap ? true : false;

    if (m_GaussianSplatEmissionProxies != nullptr && m_GaussianSplatEmissionIntensity > 0.0f)
    {
        const std::vector<GaussianSplatEmissionProxy>& proxies = *m_GaussianSplatEmissionProxies;
        for (uint32_t proxyIndex = 0; proxyIndex < proxies.size() && lightBufferOffset < m_MaxLightsInBuffer; ++proxyIndex)
        {
            const PolymorphicLightInfoFull polymorphicLight = ConvertGaussianSplatEmissionProxy(
                proxies[proxyIndex],
                m_GaussianSplatEmissionObjectToWorld,
                m_GaussianSplatEmissionIntensity,
                proxyIndex);

            PrepareLightsTask task;
            task.instanceAndGeometryIndex = TASK_PRIMITIVE_LIGHT_BIT | uint32_t(primitiveLightInfos.size());
            task.lightBufferOffset = lightBufferOffset;
            task.triangleCount = 1;
            task.previousLightBufferOffset = -1;

            lightBufferOffset += task.triangleCount;

            tasks.push_back(task);
            primitiveLightInfos.push_back(polymorphicLight);
            numFinitePrimLights++;
        }
    }

    for (const scene::LightRenderProxy* lightProxy : sortedLights)
    {
        PolymorphicLightInfoFull polymorphicLight = {};
        if (!ConvertLightProxy(*lightProxy, polymorphicLight, enableImportanceSampledEnvironmentLight, m_EnvironmentMap))
            continue;

        auto pOffset = m_PrimitiveLightBufferOffsets.find(lightProxy->entity);

        PrepareLightsTask task;
        task.instanceAndGeometryIndex = TASK_PRIMITIVE_LIGHT_BIT | uint32_t(primitiveLightInfos.size());
        task.lightBufferOffset = lightBufferOffset;
        task.triangleCount = 1;
        task.previousLightBufferOffset = (pOffset != m_PrimitiveLightBufferOffsets.end()) ? pOffset->second : -1;

        m_PrimitiveLightBufferOffsets[lightProxy->entity] = lightBufferOffset;

        lightBufferOffset += task.triangleCount;

        tasks.push_back(task);
        primitiveLightInfos.push_back(polymorphicLight);

        if (lightInfinityRank(*lightProxy) != 0)
            numInfinitePrimLights++;
        else
            numFinitePrimLights++;
    }

	lightBufferParams.localLightBufferRegion.numLights += numFinitePrimLights;
	lightBufferParams.infiniteLightBufferRegion.firstLightIndex = lightBufferParams.localLightBufferRegion.numLights;
    // Note we do not include the environment map in numInfiniteLights
	lightBufferParams.infiniteLightBufferRegion.numLights = numInfinitePrimLights - enableImportanceSampledEnvironmentLight;;
	lightBufferParams.environmentLightParams.lightIndex = lightBufferParams.infiniteLightBufferRegion.firstLightIndex + lightBufferParams.infiniteLightBufferRegion.numLights;
	lightBufferParams.environmentLightParams.lightPresent = enableImportanceSampledEnvironmentLight ? 1u : 0u;
    
    commandList->writeBuffer(m_TaskBuffer, tasks.data(), tasks.size() * sizeof(PrepareLightsTask));

    if (!primitiveLightInfos.empty())
    {
        commandList->writeBuffer(m_PrimitiveLightBuffer, primitiveLightInfos.data(), primitiveLightInfos.size() * sizeof(PolymorphicLightInfoFull));
    }

    // clear the mapping buffer - value of 0 means all mappings are invalid
    commandList->clearBufferUInt(m_LightIndexMappingBuffer, 0);

    // clear the PDF texture mip 0 - not all of it might be written by this shader
    commandList->clearTextureFloat(m_LocalLightPdfTexture, 
        caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), 
        caustica::rhi::Color(0.f));

    caustica::rhi::ComputeState state;
    state.pipeline = m_computePipeline;
    state.bindings = { m_bindingSet, m_descriptorTable };

    PrepareLightsConstants constants;
    constants.numTasks = uint32_t(tasks.size());
    constants.currentFrameLightOffset = m_MaxLightsInBuffer * m_OddFrame;
    constants.previousFrameLightOffset = m_MaxLightsInBuffer * !m_OddFrame;
    constants._padding = 0;
    constants.envMapSceneParams = {};
    if (enableImportanceSampledEnvironmentLight)
    {
        constants.envMapSceneParams = m_EnvironmentMapSceneParams;
        constants.envMapImportanceSamplingParams = m_EnvironmentMap->getImportanceSampling()->getShaderParams( );
    }

    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(PrepareLightsConstants));
    //commandList->setPushConstants(&constants, sizeof(constants));

    commandList->setComputeState(state);
    
    //Skip the prepare lights dispatch if there are no lights. Note the Environment map is handled in another pass
    if (lightBufferOffset > 0)
        commandList->dispatch(dm::div_ceil(lightBufferOffset, 256));

    commandList->endMarker();

	lightBufferParams.localLightBufferRegion.firstLightIndex += constants.currentFrameLightOffset;
	lightBufferParams.infiniteLightBufferRegion.firstLightIndex += constants.currentFrameLightOffset;
	lightBufferParams.environmentLightParams.lightIndex += constants.currentFrameLightOffset;
    
    m_OddFrame = !m_OddFrame;

    return lightBufferParams;
}

caustica::rhi::TextureHandle PrepareLightsPass::getEnvironmentMapTexture()
{
    return m_EnvironmentMap ? m_EnvironmentMap->getEnvMapCube() : nullptr;
}
