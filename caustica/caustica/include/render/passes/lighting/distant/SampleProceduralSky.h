// Note:
// This is a simple example of a procedural sky, used to stress test the path tracer dynamic environment map capability.
// The shaders were borrowed from https://www.shadertoy.com/view/tdSXzD by user 'stilltravelling' - much appreciated.
// 
// There's a much better version on https://www.shadertoy.com/view/cdlyWr that includes moon as well as well as moving stars.
// It would be nice to upgrade to it at some point^ with proper parametrization for earth rotation (stars moving), sun 
// position, moon position, etc.


#pragma once

#include <render/core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>

#include <math/math.h>

using namespace caustica::math;

#include <shaders/render/lighting/distant/SampleProceduralSky.hlsli>

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render { class RenderDevice; }
    struct TextureData;
    struct LoadedTexture;
}

class SampleProceduralSky 
{
public:
    SampleProceduralSky( nvrhi::IDevice* device, std::shared_ptr<caustica::TextureLoader> textureCache, caustica::render::RenderDevice& renderDevice, nvrhi::ICommandList* commandList );
    ~SampleProceduralSky();

    nvrhi::TextureHandle            GetTransmittanceTexture() const;
    nvrhi::TextureHandle            GetScatterringTexture() const;
    nvrhi::TextureHandle            GetIrradianceTexture() const;
    nvrhi::TextureHandle            GetCloudsTexture() const;
    nvrhi::TextureHandle            GetNoiseTexture() const;

    bool                            Update( double sceneTime, ProceduralSkyConstants & outConstants, const std::string & presetType, bool forceInstantUpdate );

    void                            DebugGUI(float indent);

private:
    double                          m_lastSceneTime = 0.0;

    nvrhi::DeviceHandle                             m_device;
    std::shared_ptr<caustica::TextureLoader>    m_textureCache;

    std::shared_ptr<caustica::LoadedTexture>   m_transmittanceTexture;
    std::shared_ptr<caustica::LoadedTexture>   m_scatterringTexture;
    std::shared_ptr<caustica::LoadedTexture>   m_irradianceTexture;
    std::shared_ptr<caustica::LoadedTexture>   m_cloudsTexture;
    std::shared_ptr<caustica::LoadedTexture>   m_noiseTexture;

    float3                          m_colorTint                 = float3(1.45f, 1.29f, 1.27f);
    float                           m_brightness                = 1.0f;
    float                           m_sunBrightness             = 5.0f;
    float                           m_cloudsMovementSpeed       = 0.8f;
    float                           m_timeOfDayMovementSpeed    = 300.0f;
    float                           m_sunTimeOfDayOffset        = -0.4f;
    float                           m_sunEastWestRotation       = 0.0f;
    float                           m_sunAngularDiameterDeg     = 0.5332f;

    float                           m_cloudDensityOffset        = 0.75f;
    float                           m_cloudTransmittance        = 2.5f;
    float                           m_cloudScattering           = 2.0f;

    float                           m_timeOfDayL2               = 0.0f;
    float                           m_timeOfDayL1               = 0.0f;

    ProceduralSkyConstants          m_lastConstants;
};
