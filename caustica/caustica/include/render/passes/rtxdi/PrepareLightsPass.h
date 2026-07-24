#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <rhi/rhi.h>
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
    caustica::rhi::DeviceHandle m_device;

    caustica::rhi::ShaderHandle m_computeShader;
    caustica::rhi::ComputePipelineHandle m_computePipeline;
    caustica::rhi::BindingLayoutHandle m_bindingLayout;
    caustica::rhi::BindingSetHandle m_bindingSet;
    caustica::rhi::BindingLayoutHandle m_bindlessLayout;

    caustica::rhi::BufferHandle m_TaskBuffer;
    caustica::rhi::BufferHandle m_PrimitiveLightBuffer;
    caustica::rhi::BufferHandle m_LightIndexMappingBuffer;
    caustica::rhi::BufferHandle m_GeometryInstanceToLightBuffer;
    caustica::rhi::TextureHandle m_LocalLightPdfTexture;

    // Non-owning: scene lighting owns EnvMapProcessor. Holding a shared_ptr here
    // keeps it alive until RtxdiPass teardown and can unload textures after AssetSystem.
    EnvMapProcessor* m_EnvironmentMap = nullptr;
    EnvMapSceneParams m_EnvironmentMapSceneParams;

    caustica::rhi::BufferHandle m_constantBuffer;

    uint32_t m_MaxLightsInBuffer;
    bool m_OddFrame = false;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    caustica::render::RenderDevice& m_renderDevice;
    const caustica::scene::SceneRenderData* m_renderData = nullptr;
    size_t m_geometryInstanceCount = 0;
    caustica::rhi::DescriptorTable* m_descriptorTable = nullptr;
    caustica::render::SceneGpuFrameHandles m_gpuHandles{};
    std::shared_ptr<class MaterialGpuCache> m_materialGpuCache;
    std::shared_ptr<class OpacityMicromapBuilder> m_opacityMicromapBuilder;
    caustica::rhi::BufferHandle m_subInstanceData;
    const std::vector<GaussianSplatEmissionProxy>* m_GaussianSplatEmissionProxies = nullptr;
    caustica::math::float4x4 m_GaussianSplatEmissionObjectToWorld = caustica::math::float4x4::identity();
    float m_GaussianSplatEmissionIntensity = 0.0f;

    std::unordered_map<size_t, uint32_t> m_InstanceLightBufferOffsets; // hash(instance*, geometryIndex) -> bufferOffset
    std::unordered_map<caustica::ecs::Entity, uint32_t> m_PrimitiveLightBufferOffsets;

    std::shared_ptr<ShaderDebug>   m_shaderDebug;

public:
    PrepareLightsPass(
        caustica::rhi::Device* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        caustica::render::RenderDevice& renderDevice,
        std::shared_ptr<class MaterialGpuCache> materialGpuCache,
        std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder,
        caustica::rhi::BufferHandle subInstanceData,
        caustica::rhi::BindingLayout* bindlessLayout,
        std::shared_ptr<ShaderDebug> shaderDebug
    );

    void setFrameInputs(
        const caustica::scene::SceneRenderData* renderData,
        size_t geometryInstanceCount,
        caustica::rhi::DescriptorTable* descriptorTable,
        const caustica::render::SceneGpuFrameHandles& gpuHandles,
        EnvMapProcessor* environmentMap = nullptr,
        EnvMapSceneParams envMapSceneParams = {});
    void setGaussianSplatEmissionProxies(const std::vector<GaussianSplatEmissionProxy>* proxies, caustica::math::float4x4 objectToWorld, float emissionIntensity);
    void createPipeline();
    void createBindingSet(RtxdiResources& resources, const RenderTargets& renderTargets);
    void countLightsInScene(uint32_t& numEmissiveMeshes, uint32_t& numEmissiveTriangles);

    RTXDI_LightBufferParameters process(caustica::rhi::CommandList* commandList);

    caustica::rhi::TextureHandle getEnvironmentMapTexture();
};
