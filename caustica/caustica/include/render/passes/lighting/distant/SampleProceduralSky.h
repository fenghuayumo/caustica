// Hillaire 2020 (UE Sky Atmosphere) procedural sky — runtime LUT generation + cubemap bake source.
#pragma once

#include <rhi/nvrhi.h>
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
        nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~SampleProceduralSky() = default;

    nvrhi::TextureHandle GetTransmittanceTexture() const { return m_transmittanceLut; }
    nvrhi::TextureHandle GetSkyViewTexture() const { return m_skyViewLut; }
    bool Update(
        nvrhi::ICommandList* commandList,
        double sceneTime,
        ProceduralSkyConstants& outConstants,
        const std::string& presetType,
        bool forceInstantUpdate);

    // Returns true if any tuned parameter changed (caller should reset accumulation).
    bool DebugGUI(float indent);
    void ReloadShaders(std::shared_ptr<caustica::ShaderFactory> shaderFactory);

    bool IsAerialPerspectiveEnabled() const { return m_aerialPerspectiveEnabled; }
    void ApplyAerialPerspective(
        nvrhi::ICommandList* commandList,
        nvrhi::ITexture* color,
        nvrhi::ITexture* depth,
        const caustica::IView& view,
        uint width,
        uint height,
        const float3& environmentTint,
        float environmentIntensity,
        const float3& environmentRotationDeg);

private:
    void CreateLutResources();
    void FillEarthAtmosphere(AtmosphereParameters& atm) const;
    void DispatchLutPasses(nvrhi::ICommandList* commandList, const ProceduralSkyConstants& consts, bool rebuildAtmosphereLuts, bool rebuildSkyView);
    float3 ComputeSunDirection(float elevationDeg, float azimuthDeg) const;
    void ApplySunPreset(float elevationDeg, float azimuthDeg);

    double m_lastSceneTime = 0.0;

    nvrhi::DeviceHandle m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    nvrhi::TextureHandle m_transmittanceLut;
    nvrhi::TextureHandle m_multiScatLut;
    nvrhi::TextureHandle m_skyViewLut;
    nvrhi::TextureHandle m_blackLut;

    nvrhi::BufferHandle m_lutConstantBuffer;
    nvrhi::SamplerHandle m_linearClampSampler;

    nvrhi::ShaderHandle m_transmittanceCS;
    nvrhi::ShaderHandle m_multiScattCS;
    nvrhi::ShaderHandle m_skyViewCS;
    nvrhi::ComputePipelineHandle m_transmittancePSO;
    nvrhi::ComputePipelineHandle m_multiScattPSO;
    nvrhi::ComputePipelineHandle m_skyViewPSO;
    nvrhi::BindingLayoutHandle m_lutBindingLayout;
    nvrhi::BindingSetHandle m_transmittanceBindings;
    nvrhi::BindingSetHandle m_multiScatBindings;
    nvrhi::BindingSetHandle m_skyViewBindings;

    nvrhi::BufferHandle m_aerialPerspectiveConstantBuffer;
    nvrhi::ShaderHandle m_aerialPerspectiveCS;
    nvrhi::ComputePipelineHandle m_aerialPerspectivePSO;
    nvrhi::BindingLayoutHandle m_aerialPerspectiveBindingLayout;
    nvrhi::BindingSetHandle m_aerialPerspectiveBindings;

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
