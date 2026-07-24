// Hillaire 2020 (UE Sky Atmosphere) procedural sky — runtime LUT generation + cubemap bake source.
#pragma once

#include <rhi/rhi.h>
#include <math/math.h>
#include <memory>
#include <string>

using namespace caustica::math;

#include <shaders/render/lighting/distant/SampleProceduralSky.hlsli>

namespace caustica
{
    class IView;
    class ShaderFactory;
}

class SampleProceduralSky
{
public:
    SampleProceduralSky(
        caustica::rhi::Device* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~SampleProceduralSky() = default;

    caustica::rhi::TextureHandle getTransmittanceTexture() const { return m_transmittanceLut; }
    caustica::rhi::TextureHandle getSkyViewTexture() const { return m_skyViewLut; }
    bool update(
        caustica::rhi::CommandList* commandList,
        double sceneTime,
        ProceduralSkyConstants& outConstants,
        const std::string& presetType,
        bool forceInstantUpdate);

    // Returns true if any tuned parameter changed (caller should reset accumulation).
    bool debugGUI(float indent);
    void reloadShaders(std::shared_ptr<caustica::ShaderFactory> shaderFactory);

    bool isAerialPerspectiveEnabled() const { return m_aerialPerspectiveEnabled; }
    void applyAerialPerspective(
        caustica::rhi::CommandList* commandList,
        caustica::rhi::Texture* color,
        caustica::rhi::Texture* depth,
        const caustica::IView& view,
        uint width,
        uint height,
        const float3& environmentTint,
        float environmentIntensity,
        const float3& environmentRotationDeg);

private:
    void createLutResources();
    void fillEarthAtmosphere(AtmosphereParameters& atm) const;
    void dispatchLutPasses(caustica::rhi::CommandList* commandList, const ProceduralSkyConstants& consts, bool rebuildAtmosphereLuts, bool rebuildSkyView);
    float3 computeSunDirection(float elevationDeg, float azimuthDeg) const;
    void applySunPreset(float elevationDeg, float azimuthDeg);

    double m_lastSceneTime = 0.0;

    caustica::rhi::DeviceHandle m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    caustica::rhi::TextureHandle m_transmittanceLut;
    caustica::rhi::TextureHandle m_multiScatLut;
    caustica::rhi::TextureHandle m_skyViewLut;
    caustica::rhi::TextureHandle m_blackLut;

    caustica::rhi::BufferHandle m_lutConstantBuffer;
    caustica::rhi::SamplerHandle m_linearClampSampler;

    caustica::rhi::ShaderHandle m_transmittanceCS;
    caustica::rhi::ShaderHandle m_multiScattCS;
    caustica::rhi::ShaderHandle m_skyViewCS;
    caustica::rhi::ComputePipelineHandle m_transmittancePSO;
    caustica::rhi::ComputePipelineHandle m_multiScattPSO;
    caustica::rhi::ComputePipelineHandle m_skyViewPSO;
    caustica::rhi::BindingLayoutHandle m_lutBindingLayout;
    caustica::rhi::BindingSetHandle m_transmittanceBindings;
    caustica::rhi::BindingSetHandle m_multiScatBindings;
    caustica::rhi::BindingSetHandle m_skyViewBindings;

    caustica::rhi::BufferHandle m_aerialPerspectiveConstantBuffer;
    caustica::rhi::ShaderHandle m_aerialPerspectiveCS;
    caustica::rhi::ComputePipelineHandle m_aerialPerspectivePSO;
    caustica::rhi::BindingLayoutHandle m_aerialPerspectiveBindingLayout;
    caustica::rhi::BindingSetHandle m_aerialPerspectiveBindings;

    bool m_atmosphereLutsValid = false;
    AtmosphereParameters m_lastAtmosphere = {};
    float3 m_lastSunDir = float3(0, 0, 0);
    float3 m_lastSunIlluminance = float3(0, 0, 0);
    float m_lastCameraHeightKm = -1.0f;

    // Sun direction (sky-local Z-up): elevation 0=horizon, 90=zenith; azimuth 0=+X, 90=+Y.
    float m_sunElevationDeg = 35.0f;
    float m_sunAzimuthDeg = 25.0f;
    float m_noonAzimuthDeg = 180.0f;   // day-cycle noon bearing when animate is on
    bool m_animateSun = false;
    float m_sunAnimSpeed = 1.0f;       // day cycles per real-time minute
    float m_sunAnimMaxElevation = 65.0f;
    float m_sunAnimPhase = 0.25f;      // 0..1 within day

    float m_sunBrightness = 1.0f;
    float m_sunAngularDiameterDeg = 0.5332f;
    float m_multiScatteringFactor = 1.0f;
    float m_cameraHeightKm = 0.15f;

    // Physical atmosphere / aerosol controls. Coefficient values are Earth-relative multipliers.
    float m_atmosphereHeightKm = 100.0f;
    float m_rayleighScatteringScale = 1.0f;
    float m_rayleighHeightKm = 8.0f;
    float m_mieScatteringScale = 1.0f;
    float m_mieAbsorptionScale = 1.0f;
    float m_mieHeightKm = 1.2f;
    float m_mieAnisotropy = 0.8f;
    float m_ozoneScale = 1.0f;
    float3 m_groundAlbedo = float3(0.3f, 0.3f, 0.3f);

    bool m_aerialPerspectiveEnabled = true;
    float m_worldToKilometers = 0.001f;
    float m_aerialPerspectiveMaxDistanceKm = 100.0f;
    int m_aerialPerspectiveSampleCount = 12;

    // Smoothed targets for preset transitions
    float m_sunElevationL1 = 35.0f;
    float m_sunElevationL2 = 35.0f;
    float m_sunAzimuthL1 = 25.0f;
    float m_sunAzimuthL2 = 25.0f;

    std::string m_activePresetType;
    ProceduralSkyConstants m_lastConstants = {};
};
