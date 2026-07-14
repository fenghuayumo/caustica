#pragma once

#include <render/core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>

#include <math/math.h>

#include <filesystem>

using namespace caustica::math;

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <shaders/render/lighting/distant/EnvMapImportanceSamplingCache.hlsl>

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    struct ImageAsset;
}
namespace caustica::render
{
    class MipMapGenPass;
}

// This is used to 
//  * pre-process importance sampling for a given cubemap source (baked by EnvMapProcessor).
//  * provide all buffers and constants required for importance sampling the environment map.
// It supports 3+ approaches:
//  - uniform reference
//  - classic MIP descent (implementation originates in https://github.com/NVIDIAGameWorks/Falcor)
//  - presampled lights (use MIP descent to pre-generate a bunch of lights each frame)
class EnvMapImportanceSamplingCache
{
public:

public:
    EnvMapImportanceSamplingCache( nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory );
    ~EnvMapImportanceSamplingCache();

    void                            createRenderPasses();

    void                            preUpdate(nvrhi::TextureHandle sourceCubemap, bool newSource);
    void                            update(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap);

    int                             getImportanceMapResolution();
    int                             getImportanceMapMIPLevels();
    nvrhi::TextureHandle            getImportanceMapOnly() const        { return m_importanceMapTexture; }
    nvrhi::TextureHandle            getRadianceAndImportanceMap() const { return m_radianceMapTexture; }
    
    // TODO: this will be obsolete and will be removed
    void                            executePresampling(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap, int sampleIndex);
    //nvrhi::BufferHandle             getPresampledBuffer() const { return m_presampledBuffer; }

    nvrhi::SamplerHandle            getImportanceMapSampler() const { return m_pointClampSampler; }

    // nvrhi::TextureHandle            getEnvMapCube() const           { return m_cubemap; }

    bool                            debugGUI(float indent);

    EnvMapImportanceSamplingParams  getShaderParams()                   { return m_envMapImportanceSamplingParams; }

private:
    void                            createImportanceMap();
    void                            generateImportanceMap(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap);
    void                            fillCacheConsts(EnvMapImportanceSamplingCacheConstants & constants, nvrhi::TextureHandle sourceCubemap, int sampleIndex);

private:
    nvrhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    nvrhi::SamplerHandle            m_pointClampSampler;
    nvrhi::SamplerHandle            m_linearWrapSampler;

    nvrhi::BufferHandle             m_builderConstants;  // EnvMapImportanceSamplingCacheConstants

    // MIP hierarchy needed for MIP descent importance sampling approach (always needed)
    nvrhi::TextureHandle            m_importanceMapTexture;
    nvrhi::TextureHandle            m_radianceMapTexture;
    nvrhi::ShaderHandle             m_importanceMapComputeShader;
    nvrhi::BindingLayoutHandle      m_importanceMapBindingLayout;
    nvrhi::ComputePipelineHandle    m_importanceMapPipeline;
    nvrhi::BindingSetHandle         m_importanceMapBindingSet;
    std::shared_ptr<caustica::render::MipMapGenPass> m_MIPMapPass;
    std::shared_ptr<caustica::render::MipMapGenPass> m_MIPMapPassRad;  // for m_radianceMapTexture - should be combined with importance map mip gen

#if 0
    // Pre-sampling approach (faster for path tracing, but limited)
    nvrhi::BufferHandle             m_presampledBuffer;
    nvrhi::ShaderHandle             m_presamplingCS;
    nvrhi::BindingLayoutHandle      m_presamplingBindingLayout;
    nvrhi::ComputePipelineHandle    m_presamplingPipeline;
    nvrhi::BindingSetHandle         m_presamplingBindingSet;
#endif

    EnvMapImportanceSamplingParams  m_envMapImportanceSamplingParams;
};
