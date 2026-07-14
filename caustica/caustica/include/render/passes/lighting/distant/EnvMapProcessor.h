#pragma once

#include <assets/Handle.h>
#include <assets/ImageAsset.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>

#include <math/math.h>

#include <filesystem>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>

using namespace caustica::math;
using namespace caustica;

#include <shaders/render/lighting/distant/EnvMapProcessor.hlsl>
#include <shaders/render/lighting/distant/BRDFLUTGenerator.hlsl>

#include "SampleProceduralSky.h"

namespace caustica
{
    class BindingCache;
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render { class RenderDevice; }
    struct ImageAsset;
}

class ShaderDebug;
class EnvMapImportanceSamplingCache;
class ComputePipelineRegistry;
class ComputeShaderVariant;

// This is used to bake the cubemap with the given inputs. Inputs can be equirectangular envmap image or procedural sky, and directional lights.
// Also provides shared cubemap processing functionality (mips, GGX filtering, SH projection) via processCubemap().
class EnvMapProcessor 
{
public:
    struct UpdateSettings
    {
        // Use this if input envmap is FP32 and outside of max FP16 (65504.0, which is the max we support for perf reasons) - you can premultiply with say 1/16 to avoid clamping 
        // and later use envmap sampling Intensity setting of 16 to offset! This will also help with baking in small sharp bright directional lights. Values lower than (roughly) 1/1024 can result in image quality loss.
        float   EnvMapRadianceScale = 1.0;  

        //BakeSettings() {}
        //BakeSettings(const float envMapRadianceScale) : EnvMapRadianceScale(envMapRadianceScale) {}
    };

    // Options for processCubemap() - shared cubemap processing
    struct CubemapProcessingOptions
    {
        bool generateMips = true;           // Solid-angle weighted mip chain
        bool ggxPrefilter = false;          // GGX specular convolution (for IBL specular)
        bool projectToSH = false;           // Project to SH and reproject to low-res diffuse cube
        bool bc6hCompress = false;          // BC6H compression
        bool generateImportanceMap = false; // For path tracer importance sampling
    };

    // Results from processCubemap() - caller provides the destination textures
    struct CubemapProcessingResults
    {
        nvrhi::TextureHandle filteredCubemap;       // GGX-filtered mip chain (for specular)
        nvrhi::TextureHandle diffuseIrradianceCube; // Low-res SH-reprojected cubemap (for diffuse)
    };

    constexpr static uint           c_MaxDirLights  = EMB_MAXDIRLIGHTS;    // Can't have any more than this number of directional lights baked into cubemap, sorry.
    constexpr static uint           c_BRDFLUTSize   = BRDF_LUT_SIZE;       // Size of the BRDF integration LUT
    constexpr static uint           c_IrradianceCubeSize = 32;             // Size of diffuse irradiance cubemap
    

public:
    EnvMapProcessor( nvrhi::IDevice* device, std::shared_ptr<caustica::TextureLoader> textureCache, bool enableRasterPrecompute );
    ~EnvMapProcessor();

    void                            sceneReloaded()                 { m_targetResolution = 0; } // change default target resolution on each scene load

    void                            createRenderPasses(std::shared_ptr<ShaderDebug> shaderDebug, std::shared_ptr<caustica::ShaderFactory> shaderFactory, std::shared_ptr<ComputePipelineRegistry> computePipelineRegistry);

    void                            preUpdate( nvrhi::ICommandList* commandList, caustica::render::RenderDevice& renderDevice, std::string envMapBackgroundPath, const std::filesystem::path& sceneDirectory = std::filesystem::path() );
    bool                            update( nvrhi::ICommandList * commandList, caustica::BindingCache & bindingCache, caustica::render::RenderDevice& renderDevice, const UpdateSettings & settings, double sceneTime, EMB_DirectionalLight const * directionalLights, uint directionaLightCount, bool forceInstantUpdate );

    nvrhi::TextureHandle            getEnvMapCube() const           { return (m_outputIsCompressed)?(m_cubemapBC6H):(m_cubemap); }
    nvrhi::SamplerHandle            getEnvMapCubeSampler() const    { return m_linearSampler; }
    uint                            getEnvMapCubeDim() const        { return m_cubeDim; }
    uint64_t                        getEnvMapVersion() const        { return m_versionID; }

    bool                            debugGUI(float indent);

    bool                            isProcedural() const            { return isProceduralSky( m_loadedSourceBackgroundPath.c_str() ); }
    const std::shared_ptr<SampleProceduralSky> &
                                    getProceduralSky() const        { return m_proceduralSky; }

    void                            setTargetCubeResolution(uint res)   { m_targetResolution = res; }
    int                             getTargetCubeResolution() const;

    const std::shared_ptr<EnvMapImportanceSamplingCache>
                                    getImportanceSampling() const   { return m_importanceSamplingCache; }

    // BRDF LUT for split-sum IBL approximation (generated once at startup)
    nvrhi::TextureHandle            getBRDFLUT() const              { return m_brdfLUT; }
    bool                            isBRDFLUTReady() const          { return m_brdfLUT != nullptr && m_brdfLUTGenerated; }

    // process an external cubemap with the specified options
    // This allows reusing the mip generation, GGX filtering, and SH projection for external cubemaps (e.g., local RT cubemap)
    void                            processCubemap(
                                        nvrhi::ICommandList* commandList,
                                        caustica::BindingCache& bindingCache,
                                        nvrhi::TextureHandle sourceCubemap,
                                        const CubemapProcessingOptions& options,
                                        const CubemapProcessingResults& results);
    
    // Generate the BRDF integration LUT (should be called once during initialization)
    bool                            generateBRDFLUT(nvrhi::ICommandList* commandList, caustica::BindingCache& bindingCache);
    
private:
    void                            ggxPrefilterCubemap(nvrhi::ICommandList* commandList, caustica::BindingCache& bindingCache, 
                                        nvrhi::TextureHandle srcCubemap, nvrhi::TextureHandle dstCubemap);
    void                            convolveDiffuseIrradiance(nvrhi::ICommandList* commandList, caustica::BindingCache& bindingCache,
                                        nvrhi::TextureHandle srcCubemap, nvrhi::TextureHandle dstCubemap);
    void                            generateCubemapMips(nvrhi::ICommandList* commandList, caustica::BindingCache& bindingCache, 
                                        nvrhi::TextureHandle cubemap);

    void                            initBuffers(uint cubeDim);
    void                            unloadSourceBackgrounds();

private:
    nvrhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::TextureLoader> m_textureCache;
    std::shared_ptr<ShaderDebug>    m_shaderDebug;

    nvrhi::ShaderHandle             m_baseLayerCS;
    nvrhi::ComputePipelineHandle    m_baseLayerPSO;

    nvrhi::ShaderHandle             m_MIPReduceCS;
    nvrhi::ComputePipelineHandle    m_MIPReducePSO;

    nvrhi::BindingLayoutHandle      m_commonBindingLayout;
    nvrhi::BindingLayoutHandle      m_reduceBindingLayout;

    bool                            m_BC6UCompressionEnabled = true;
    nvrhi::ShaderHandle             m_BC6UCompressLowCS;
    nvrhi::ComputePipelineHandle    m_BC6UCompressLowPSO;
    nvrhi::ShaderHandle             m_BC6UCompressHighCS;
    nvrhi::ComputePipelineHandle    m_BC6UCompressHighPSO;
    nvrhi::BindingLayoutHandle      m_BC6UCompressBindingLayout;

    nvrhi::BufferHandle             m_constantBuffer;

    nvrhi::SamplerHandle            m_pointSampler;
    nvrhi::SamplerHandle            m_linearSampler;
    nvrhi::SamplerHandle            m_linearClampSampler;
    nvrhi::SamplerHandle            m_equiRectSampler;

    std::string                     m_sourceBackgroundPath;
    std::string                     m_loadedSourceBackgroundPath;
    caustica::Handle<caustica::ImageAsset>  m_loadedSourceBackgroundTextureEquirect;
    caustica::Handle<caustica::ImageAsset>  m_loadedSourceBackgroundTextureCubemap;

    nvrhi::TextureHandle            m_cubemap;
    nvrhi::TextureDesc              m_cubemapDesc;
    nvrhi::TextureHandle            m_cubemapBC6H;
    nvrhi::TextureHandle            m_cubemapBC6HScratch;
    uint                            m_cubeDim = 0;

    uint                            m_targetResolution = 0;

    uint64_t                        m_versionID = -1;
    bool                            m_renderPassesDirty = true;

    EMB_DirectionalLight            m_bakedLights[c_MaxDirLights];
    uint                            m_bakedLightCount = 0;

    int                             m_compressionQuality = 1;       // 0 - disabled; 1 - low quality; 2 - high quality
    bool                            m_outputIsCompressed = false;   // updated in update() - it reflects current state of textures while 'm_compressionQuality' reflects required (future) state

    std::shared_ptr<SampleProceduralSky>
                                    m_proceduralSky;
    bool                            m_dbgForceDynamic = false;

    std::string                     m_dbgSaveBaked = "";

    std::shared_ptr<EnvMapImportanceSamplingCache>
                                    m_importanceSamplingCache;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    bool                            m_enableRasterPrecompute = false;

    // BRDF LUT for split-sum IBL
    nvrhi::TextureHandle            m_brdfLUT;
    nvrhi::ShaderHandle             m_brdfLUTCS;
    nvrhi::ComputePipelineHandle    m_brdfLUTPSO;
    nvrhi::BindingLayoutHandle      m_brdfLUTBindingLayout;
    nvrhi::BufferHandle             m_brdfLUTConstantBuffer;
    bool                            m_brdfLUTGenerated = false;

    // GGX pre-filtering for specular IBL - managed by ComputePipelineRegistry for hot reload
    std::shared_ptr<ComputeShaderVariant> m_ggxPrefilterVariant;
    nvrhi::BindingLayoutHandle      m_ggxPrefilterBindingLayout;

    // Diffuse irradiance convolution (SH-based) - managed by ComputePipelineRegistry for hot reload
    std::shared_ptr<ComputeShaderVariant> m_irradianceConvolveVariant;
    nvrhi::BindingLayoutHandle      m_irradianceConvolveBindingLayout;

    // Reference to compute pipeline baker for hot reload support
    std::shared_ptr<ComputePipelineRegistry> m_computePipelineRegistry;
};
