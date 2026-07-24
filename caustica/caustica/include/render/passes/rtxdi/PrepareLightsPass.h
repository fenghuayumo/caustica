#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <rhi/nvrhi.h>
#include <rtxdi/DI/ReSTIRDI.h>
#include <memory>
#include <unordered_map>
#include <vector>

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/SceneGpuResources.h>

namespace caustica
{
    namespace render { class RenderDevice; }
    namespace scene { class SceneRenderData; }
    class ShaderFactory;
}

class RenderTargets;
class RtxdiResources;
class EnvMapProcessor;
class ShaderDebug;

class PrepareLightsPass
{
private:
    nvrhi::DeviceHandle m_device;

    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BindingLayoutHandle m_bindlessLayout;

    nvrhi::BufferHandle m_TaskBuffer;
    nvrhi::BufferHandle m_PrimitiveLightBuffer;
    nvrhi::BufferHandle m_LightIndexMappingBuffer;
    nvrhi::BufferHandle m_GeometryInstanceToLightBuffer;
    nvrhi::TextureHandle m_LocalLightPdfTexture;

    // Non-owning: scene lighting owns EnvMapProcessor. Holding a shared_ptr here
    // keeps it alive until RtxdiPass teardown and can unload textures after AssetSystem.
    EnvMapProcessor* m_EnvironmentMap = nullptr;
    EnvMapSceneParams m_EnvironmentMapSceneParams;

    nvrhi::BufferHandle m_constantBuffer;

    uint32_t m_MaxLightsInBuffer;
    bool m_OddFrame = false;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    caustica::render::RenderDevice& m_renderDevice;
    const caustica::scene::SceneRenderData* m_renderData = nullptr;
    size_t m_geometryInstanceCount = 0;
    nvrhi::IDescriptorTable* m_descriptorTable = nullptr;
    caustica::render::SceneGpuFrameHandles m_gpuHandles{};
    std::shared_ptr<class MaterialGpuCache> m_materialGpuCache;
    std::shared_ptr<class OpacityMicromapBuilder> m_opacityMicromapBuilder;
    nvrhi::BufferHandle m_subInstanceData;
    const std::vector<GaussianSplatEmissionProxy>* m_GaussianSplatEmissionProxies = nullptr;
    caustica::math::float4x4 m_GaussianSplatEmissionObjectToWorld = caustica::math::float4x4::identity();
    float m_GaussianSplatEmissionIntensity = 0.0f;

    std::unordered_map<size_t, uint32_t> m_InstanceLightBufferOffsets; // hash(instance*, geometryIndex) -> bufferOffset
    std::unordered_map<caustica::ecs::Entity, uint32_t> m_PrimitiveLightBufferOffsets;

    std::shared_ptr<ShaderDebug>   m_shaderDebug;

public:
    PrepareLightsPass(
        nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        caustica::render::RenderDevice& renderDevice,
        std::shared_ptr<class MaterialGpuCache> materialGpuCache,
        std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder,
        nvrhi::BufferHandle subInstanceData,
        nvrhi::IBindingLayout* bindlessLayout,
        std::shared_ptr<ShaderDebug> shaderDebug
    );

    void setFrameInputs(
        const caustica::scene::SceneRenderData* renderData,
        size_t geometryInstanceCount,
        nvrhi::IDescriptorTable* descriptorTable,
        const caustica::render::SceneGpuFrameHandles& gpuHandles,
        EnvMapProcessor* environmentMap = nullptr,
        EnvMapSceneParams envMapSceneParams = {});
    void setGaussianSplatEmissionProxies(const std::vector<GaussianSplatEmissionProxy>* proxies, caustica::math::float4x4 objectToWorld, float emissionIntensity);
    void createPipeline();
    void createBindingSet(RtxdiResources& resources, const RenderTargets& renderTargets);
    void countLightsInScene(uint32_t& numEmissiveMeshes, uint32_t& numEmissiveTriangles);

    RTXDI_LightBufferParameters process(nvrhi::ICommandList* commandList);

    nvrhi::TextureHandle getEnvironmentMapTexture();
};
