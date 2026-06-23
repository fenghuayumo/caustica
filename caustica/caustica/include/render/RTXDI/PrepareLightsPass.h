/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <engine/SceneGraph.h>
#include <nvrhi/nvrhi.h>
#include <rtxdi/DI/ReSTIRDI.h>
#include <memory>
#include <unordered_map>
#include <vector>

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <render/ProcessingPasses/GaussianSplatEmissionProxy.h>

namespace caustica
{
    class CommonRenderPasses;
    class ShaderFactory;
    class Light;
}

class ExtendedScene;
class RenderTargets;
class RtxdiResources;
class EnvMapBaker;
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

    std::shared_ptr<EnvMapBaker> m_EnvironmentMap;
    EnvMapSceneParams m_EnvironmentMapSceneParams;

    nvrhi::BufferHandle m_constantBuffer;

    uint32_t m_MaxLightsInBuffer;
    bool m_OddFrame = false;
    
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
    std::shared_ptr<ExtendedScene> m_Scene;
    std::shared_ptr<class MaterialsBaker> m_materialsBaker;
    std::shared_ptr<class OmmBaker> m_ommBaker;
    nvrhi::BufferHandle m_subInstanceData;
    const std::vector<GaussianSplatEmissionProxy>* m_GaussianSplatEmissionProxies = nullptr;
    caustica::math::float4x4 m_GaussianSplatEmissionObjectToWorld = caustica::math::float4x4::identity();
    float m_GaussianSplatEmissionIntensity = 0.0f;

    std::unordered_map<size_t, uint32_t> m_InstanceLightBufferOffsets; // hash(instance*, geometryIndex) -> bufferOffset
    std::unordered_map<const caustica::Light*, uint32_t> m_PrimitiveLightBufferOffsets;

    std::shared_ptr<ShaderDebug>   m_shaderDebug;

public:
    PrepareLightsPass(
        nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        std::shared_ptr<caustica::CommonRenderPasses> commonPasses,
        std::shared_ptr<ExtendedScene> scene,
        std::shared_ptr<class MaterialsBaker> materialsBaker,
        std::shared_ptr<class OmmBaker> ommBaker,
        nvrhi::BufferHandle subInstanceData,
        nvrhi::IBindingLayout* bindlessLayout,
        std::shared_ptr<ShaderDebug> shaderDebug
    );

    void SetScene(std::shared_ptr<ExtendedScene> scene, std::shared_ptr<EnvMapBaker> environmentMap = nullptr, EnvMapSceneParams envMapSceneParams = {} );
    void SetGaussianSplatEmissionProxies(const std::vector<GaussianSplatEmissionProxy>* proxies, caustica::math::float4x4 objectToWorld, float emissionIntensity);
    void CreatePipeline();
    void CreateBindingSet(RtxdiResources& resources, const RenderTargets& renderTargets);
    void CountLightsInScene(uint32_t& numEmissiveMeshes, uint32_t& numEmissiveTriangles);
    
    RTXDI_LightBufferParameters Process(nvrhi::ICommandList* commandList);

    nvrhi::TextureHandle GetEnvironmentMapTexture();
};
