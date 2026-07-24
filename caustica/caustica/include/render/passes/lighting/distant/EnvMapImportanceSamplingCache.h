#pragma once

#include <render/core/BindingCache.h>
#include <rhi/rhi.h>
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
    EnvMapImportanceSamplingCache( caustica::rhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory );
    ~EnvMapImportanceSamplingCache();

    void                            createRenderPasses();

    void                            preUpdate(caustica::rhi::TextureHandle sourceCubemap, bool newSource);
    void                            update(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap);

    int                             getImportanceMapResolution();
    int                             getImportanceMapMIPLevels();
    caustica::rhi::TextureHandle            getImportanceMapOnly() const        { return m_importanceMapTexture; }
    caustica::rhi::TextureHandle            getRadianceAndImportanceMap() const { return m_radianceMapTexture; }
    
    // TODO: this will be obsolete and will be removed
    void                            executePresampling(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap, int sampleIndex);
    //caustica::rhi::BufferHandle             getPresampledBuffer() const { return m_presampledBuffer; }

    caustica::rhi::SamplerHandle            getImportanceMapSampler() const { return m_pointClampSampler; }

    // caustica::rhi::TextureHandle            getEnvMapCube() const           { return m_cubemap; }

    bool                            debugGUI(float indent);

    EnvMapImportanceSamplingParams  getShaderParams()                   { return m_envMapImportanceSamplingParams; }

private:
    void                            createImportanceMap();
    void                            generateImportanceMap(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap);
    void                            fillCacheConsts(EnvMapImportanceSamplingCacheConstants & constants, caustica::rhi::TextureHandle sourceCubemap, int sampleIndex);

private:
    caustica::rhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    caustica::rhi::SamplerHandle            m_pointClampSampler;
    caustica::rhi::SamplerHandle            m_linearWrapSampler;

    caustica::rhi::BufferHandle             m_builderConstants;  // EnvMapImportanceSamplingCacheConstants

    // MIP hierarchy needed for MIP descent importance sampling approach (always needed)
    caustica::rhi::TextureHandle            m_importanceMapTexture;
    caustica::rhi::TextureHandle            m_radianceMapTexture;
    caustica::rhi::ShaderHandle             m_importanceMapComputeShader;
    caustica::rhi::BindingLayoutHandle      m_importanceMapBindingLayout;
    caustica::rhi::ComputePipelineHandle    m_importanceMapPipeline;
    caustica::rhi::BindingSetHandle         m_importanceMapBindingSet;
    std::shared_ptr<caustica::render::MipMapGenPass> m_MIPMapPass;
    std::shared_ptr<caustica::render::MipMapGenPass> m_MIPMapPassRad;  // for m_radianceMapTexture - should be combined with importance map mip gen

#if 0
    // Pre-sampling approach (faster for path tracing, but limited)
    caustica::rhi::BufferHandle             m_presampledBuffer;
    caustica::rhi::ShaderHandle             m_presamplingCS;
    caustica::rhi::BindingLayoutHandle      m_presamplingBindingLayout;
    caustica::rhi::ComputePipelineHandle    m_presamplingPipeline;
    caustica::rhi::BindingSetHandle         m_presamplingBindingSet;
#endif

    EnvMapImportanceSamplingParams  m_envMapImportanceSamplingParams;
};
