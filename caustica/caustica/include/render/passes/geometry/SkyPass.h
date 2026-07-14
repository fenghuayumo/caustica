#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>


namespace caustica::render
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

struct ProceduralSkyShaderParameters;

namespace caustica::render
{
    struct SkyParameters
    {
        dm::float3 skyColor{ 0.17f, 0.37f, 0.65f };
        dm::float3 horizonColor{ 0.50f, 0.70f, 0.92f };
        dm::float3 groundColor{ 0.62f, 0.59f, 0.55f };
        dm::float3 directionUp{ 0.f, 1.f, 0.f };
        float brightness = 0.1f; // scaler for sky brightness
        float horizonSize = 30.f; // +/- degrees
        float glowSize = 5.f; // degrees, starting from the edge of the light disk
        float glowIntensity = 0.1f; // [0-1] relative to light intensity
        float glowSharpness = 4.f; // [1-10] is the glow power exponent
        float maxLightRadiance = 100.f; // clamp for light radiance derived from its angular size, 0 = no clamp
    };

    class SkyPass
    {
    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BufferHandle m_SkyCB;
        nvrhi::BindingLayoutHandle m_RenderBindingLayout;
        nvrhi::BindingSetHandle m_RenderBindingSet;
        nvrhi::GraphicsPipelineHandle m_RenderPso;
        
        std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;

    public:
        SkyPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView);

        void render(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView,
            dm::float3 lightColor,
            float lightIrradiance,
            float lightAngularSizeDegrees,
            dm::double3 lightDirection,
            const SkyParameters& params) const;

        static void fillShaderParameters(
            dm::float3 lightColor,
            float lightIrradiance,
            float lightAngularSizeDegrees,
            dm::double3 lightDirection,
            const SkyParameters& input,
            ProceduralSkyShaderParameters& output);
    };
}