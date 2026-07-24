#include <render/passes/lighting/distant/SampleProceduralSky.h>

#include <core/path_utils.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <scene/View.h>
#include <shaders/render/lighting/distant/AerialPerspective.hlsli>
#include <rhi/utils.h>
#include <imgui/imgui_renderer.h>
#include <core/scope.h>
#include <algorithm>
#include <cstring>
#include <cmath>

using namespace caustica::math;
using namespace caustica;

namespace
{
    inline float TimeIndependentLerpF(float deltaTime, float lerpRate)
    {
        return 1.0f - expf(-fabsf(deltaTime * lerpRate));
    }

    bool AtmosphereEqual(const AtmosphereParameters& a, const AtmosphereParameters& b)
    {
        return memcmp(&a, &b, sizeof(AtmosphereParameters)) == 0;
    }

    float WrapDegrees360(float deg)
    {
        deg = std::fmod(deg, 360.0f);
        if (deg < 0.0f)
            deg += 360.0f;
        return deg;
    }

    float LerpAngleDeg(float from, float to, float t)
    {
        float delta = std::fmod(to - from + 540.0f, 360.0f) - 180.0f;
        return WrapDegrees360(from + delta * t);
    }

    float AngularDistanceDeg(float a, float b)
    {
        return fabsf(std::fmod(a - b + 540.0f, 360.0f) - 180.0f);
    }
}

SampleProceduralSky::SampleProceduralSky(
    caustica::rhi::Device* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
{
    createLutResources();
    memset(&m_lastConstants, 0, sizeof(m_lastConstants));

    static_assert(sizeof(AtmosphereParameters) == 128, "AtmosphereParameters must match HLSL cbuffer packing");
    static_assert(sizeof(SkyAtmosphereLutConstants) % 16 == 0, "SkyAtmosphereLutConstants must be 16-byte aligned");
    static_assert(sizeof(ProceduralSkyConstants) % 16 == 0, "ProceduralSkyConstants must be 16-byte aligned");
    static_assert(sizeof(AerialPerspectiveConstants) % 16 == 0, "AerialPerspectiveConstants must be 16-byte aligned");
}

void SampleProceduralSky::reloadShaders(std::shared_ptr<caustica::ShaderFactory> shaderFactory)
{
    assert(shaderFactory != nullptr);
    m_shaderFactory = std::move(shaderFactory);
    createLutResources();
}

float3 SampleProceduralSky::computeSunDirection(float elevationDeg, float azimuthDeg) const
{
    const float elev = dm::radians(dm::clamp(elevationDeg, -89.9f, 89.9f));
    const float azim = dm::radians(azimuthDeg);
    // Sky-local Z-up: elevation from horizon, azimuth from +X toward +Y.
    return normalize(float3(
        std::cos(elev) * std::cos(azim),
        std::cos(elev) * std::sin(azim),
        std::sin(elev)));
}

void SampleProceduralSky::applySunPreset(float elevationDeg, float azimuthDeg)
{
    m_sunElevationDeg = elevationDeg;
    m_sunAzimuthDeg = WrapDegrees360(azimuthDeg);
    m_noonAzimuthDeg = m_sunAzimuthDeg;
    m_sunElevationL1 = m_sunElevationL2 = m_sunElevationDeg;
    m_sunAzimuthL1 = m_sunAzimuthL2 = m_sunAzimuthDeg;
    m_animateSun = false;
}

void SampleProceduralSky::createLutResources()
{
    assert(m_shaderFactory != nullptr);

    // Layouts/textures are recreated below; drop cached binding sets that reference them.
    m_transmittanceBindings = nullptr;
    m_multiScatBindings = nullptr;
    m_skyViewBindings = nullptr;
    m_aerialPerspectiveBindings = nullptr;
    m_atmosphereLutsValid = false;

    auto makeLut = [&](uint width, uint height, const char* name) -> caustica::rhi::TextureHandle
    {
        caustica::rhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = caustica::rhi::Format::RGBA16_FLOAT;
        desc.dimension = caustica::rhi::TextureDimension::Texture2D;
        desc.debugName = name;
        desc.isUAV = true;
        desc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        return m_device->createTexture(desc);
    };

    m_transmittanceLut = makeLut((uint)SKY_ATM_TRANSMITTANCE_LUT_WIDTH, (uint)SKY_ATM_TRANSMITTANCE_LUT_HEIGHT, "SkyAtm/TransmittanceLUT");
    m_multiScatLut = makeLut((uint)SKY_ATM_MULTISCAT_LUT_RES, (uint)SKY_ATM_MULTISCAT_LUT_RES, "SkyAtm/MultiScatteringLUT");
    m_skyViewLut = makeLut((uint)SKY_ATM_SKYVIEW_LUT_WIDTH, (uint)SKY_ATM_SKYVIEW_LUT_HEIGHT, "SkyAtm/SkyViewLUT");
    m_blackLut = makeLut(1, 1, "SkyAtm/BlackLUT");

    m_lutConstantBuffer = m_device->createBuffer(caustica::rhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(SkyAtmosphereLutConstants), "SkyAtm/LutConstants", caustica::c_MaxRenderPassConstantBufferVersions * 4));

    {
        caustica::rhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(true);
        samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Clamp);
        m_linearClampSampler = m_device->createSampler(samplerDesc);
    }

    m_transmittanceCS = m_shaderFactory->createShader(
        "caustica/shaders/render/lighting/distant/SkyAtmosphereLUTs.hlsl", "TransmittanceLutCS", nullptr, caustica::rhi::ShaderType::Compute);
    m_multiScattCS = m_shaderFactory->createShader(
        "caustica/shaders/render/lighting/distant/SkyAtmosphereLUTs.hlsl", "MultiScattCS", nullptr, caustica::rhi::ShaderType::Compute);
    m_skyViewCS = m_shaderFactory->createShader(
        "caustica/shaders/render/lighting/distant/SkyAtmosphereLUTs.hlsl", "SkyViewLutCS", nullptr, caustica::rhi::ShaderType::Compute);

    {
        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_UAV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(1),
            caustica::rhi::BindingLayoutItem::Texture_SRV(2),
            caustica::rhi::BindingLayoutItem::Sampler(0),
        };
        m_lutBindingLayout = m_device->createBindingLayout(layoutDesc);
    }

    caustica::rhi::ComputePipelineDesc psoDesc;
    psoDesc.bindingLayouts = { m_lutBindingLayout };
    psoDesc.CS = m_transmittanceCS;
    m_transmittancePSO = m_device->createComputePipeline(psoDesc);
    psoDesc.CS = m_multiScattCS;
    m_multiScattPSO = m_device->createComputePipeline(psoDesc);
    psoDesc.CS = m_skyViewCS;
    m_skyViewPSO = m_device->createComputePipeline(psoDesc);

    m_aerialPerspectiveConstantBuffer = m_device->createBuffer(
        caustica::rhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(AerialPerspectiveConstants),
            "SkyAtm/AerialPerspectiveConstants",
            caustica::c_MaxRenderPassConstantBufferVersions));

    m_aerialPerspectiveCS = m_shaderFactory->createShader(
        "caustica/shaders/render/lighting/distant/AerialPerspective.hlsl",
        "AerialPerspectiveCS",
        nullptr,
        caustica::rhi::ShaderType::Compute);

    {
        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_UAV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(1),
            caustica::rhi::BindingLayoutItem::Texture_SRV(2),
            caustica::rhi::BindingLayoutItem::Sampler(0),
        };
        m_aerialPerspectiveBindingLayout = m_device->createBindingLayout(layoutDesc);
    }

    psoDesc.bindingLayouts = { m_aerialPerspectiveBindingLayout };
    psoDesc.CS = m_aerialPerspectiveCS;
    m_aerialPerspectivePSO = m_device->createComputePipeline(psoDesc);

    m_atmosphereLutsValid = false;
}

void SampleProceduralSky::fillEarthAtmosphere(AtmosphereParameters& atm) const
{
    memset(&atm, 0, sizeof(atm));
    atm.BottomRadius = 6360.0f;
    atm.TopRadius = atm.BottomRadius + m_atmosphereHeightKm;
    atm.RayleighDensityExpScale = -1.0f / m_rayleighHeightKm;
    atm.MieDensityExpScale = -1.0f / m_mieHeightKm;
    atm.RayleighScattering = float3(0.005802f, 0.013558f, 0.033100f) * m_rayleighScatteringScale;
    atm.MiePhaseG = m_mieAnisotropy;
    atm.MieScattering = float3(0.003996f, 0.003996f, 0.003996f) * m_mieScatteringScale;
    atm.MieAbsorption = float3(0.000444f, 0.000444f, 0.000444f) * m_mieAbsorptionScale;
    atm.MieExtinction = atm.MieScattering + atm.MieAbsorption;
    atm.AbsorptionDensity0LayerWidth = 25.0f;
    atm.AbsorptionDensity0ConstantTerm = -2.0f / 3.0f;
    atm.AbsorptionDensity0LinearTerm = 1.0f / 15.0f;
    atm.AbsorptionDensity1ConstantTerm = 8.0f / 3.0f;
    atm.AbsorptionDensity1LinearTerm = -1.0f / 15.0f;
    atm.AbsorptionExtinction = float3(0.000650f, 0.001881f, 0.000085f) * m_ozoneScale;
    atm.GroundAlbedo = m_groundAlbedo;
    atm.MultiScatteringFactor = m_multiScatteringFactor;
}

void SampleProceduralSky::dispatchLutPasses(
    caustica::rhi::CommandList* commandList,
    const ProceduralSkyConstants& consts,
    bool rebuildAtmosphereLuts,
    bool rebuildSkyView)
{
    SkyAtmosphereLutConstants lutConsts = {};
    lutConsts.Atmosphere = consts.Atmosphere;
    lutConsts.SunDir = consts.SunDir;
    lutConsts.CameraHeightKm = consts.CameraHeightKm;
    lutConsts.SunIlluminance = consts.SunIlluminance;
    lutConsts.MultiScatteringLUTRes = SKY_ATM_MULTISCAT_LUT_RES;
    lutConsts.TransmittanceLutWidth = (uint)SKY_ATM_TRANSMITTANCE_LUT_WIDTH;
    lutConsts.TransmittanceLutHeight = (uint)SKY_ATM_TRANSMITTANCE_LUT_HEIGHT;
    lutConsts.SkyViewLutWidth = (uint)SKY_ATM_SKYVIEW_LUT_WIDTH;
    lutConsts.SkyViewLutHeight = (uint)SKY_ATM_SKYVIEW_LUT_HEIGHT;
    commandList->writeBuffer(m_lutConstantBuffer, &lutConsts, sizeof(lutConsts));

    auto ensureBindings = [&](caustica::rhi::BindingSetHandle& cached,
        caustica::rhi::TextureHandle uav, caustica::rhi::TextureHandle srv0, caustica::rhi::TextureHandle srv1) -> caustica::rhi::BindingSetHandle
    {
        if (!cached)
        {
            caustica::rhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                caustica::rhi::BindingSetItem::ConstantBuffer(0, m_lutConstantBuffer),
                caustica::rhi::BindingSetItem::Texture_UAV(0, uav),
                caustica::rhi::BindingSetItem::Texture_SRV(0, srv0),
                caustica::rhi::BindingSetItem::Texture_SRV(1, srv1),
                caustica::rhi::BindingSetItem::Texture_SRV(2, m_blackLut),
                caustica::rhi::BindingSetItem::Sampler(0, m_linearClampSampler),
            };
            cached = m_device->createBindingSet(setDesc, m_lutBindingLayout);
        }
        return cached;
    };

    if (rebuildAtmosphereLuts)
    {
        {
            RAII_SCOPE(commandList->beginMarker("SkyAtm/TransmittanceLUT");, commandList->endMarker(););
            caustica::rhi::BindingSetHandle transmittanceBindings =
                ensureBindings(m_transmittanceBindings, m_transmittanceLut, m_blackLut, m_blackLut);
            caustica::rhi::ComputeState state;
            state.bindings = { transmittanceBindings };
            state.pipeline = m_transmittancePSO;
            commandList->setComputeState(state);
            commandList->dispatch(
                (lutConsts.TransmittanceLutWidth + 7) / 8,
                (lutConsts.TransmittanceLutHeight + 7) / 8,
                1);
            commandList->setTextureState(m_transmittanceLut, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        }

        {
            RAII_SCOPE(commandList->beginMarker("SkyAtm/MultiScatteringLUT");, commandList->endMarker(););
            caustica::rhi::BindingSetHandle multiScatBindings =
                ensureBindings(m_multiScatBindings, m_multiScatLut, m_transmittanceLut, m_blackLut);
            caustica::rhi::ComputeState state;
            state.bindings = { multiScatBindings };
            state.pipeline = m_multiScattPSO;
            commandList->setComputeState(state);
            commandList->dispatch((uint)SKY_ATM_MULTISCAT_LUT_RES, (uint)SKY_ATM_MULTISCAT_LUT_RES, 1);
            commandList->setTextureState(m_multiScatLut, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        }

        m_atmosphereLutsValid = true;
        m_lastAtmosphere = consts.Atmosphere;
    }

    if (rebuildSkyView)
    {
        RAII_SCOPE(commandList->beginMarker("SkyAtm/SkyViewLUT");, commandList->endMarker(););
        caustica::rhi::BindingSetHandle skyViewBindings =
            ensureBindings(m_skyViewBindings, m_skyViewLut, m_transmittanceLut, m_multiScatLut);
        caustica::rhi::ComputeState state;
        state.bindings = { skyViewBindings };
        state.pipeline = m_skyViewPSO;
        commandList->setComputeState(state);
        commandList->dispatch(
            (lutConsts.SkyViewLutWidth + 7) / 8,
            (lutConsts.SkyViewLutHeight + 7) / 8,
            1);
        commandList->setTextureState(m_skyViewLut, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);

        m_lastSunDir = consts.SunDir;
        m_lastSunIlluminance = consts.SunIlluminance;
        m_lastCameraHeightKm = consts.CameraHeightKm;
    }
}

void SampleProceduralSky::applyAerialPerspective(
    caustica::rhi::CommandList* commandList,
    caustica::rhi::Texture* color,
    caustica::rhi::Texture* depth,
    const caustica::IView& view,
    uint width,
    uint height,
    const float3& environmentTint,
    float environmentIntensity,
    const float3& environmentRotationDeg)
{
    if (!m_aerialPerspectiveEnabled || !commandList || !color || !depth || width == 0 || height == 0)
        return;

    AerialPerspectiveConstants constants = {};
    view.fillPlanarViewConstants(constants.View);
    constants.Atmosphere = m_lastConstants.Atmosphere;
    constants.SunDir = m_lastConstants.SunDir;
    constants.CameraHeightKm = m_lastConstants.CameraHeightKm;
    constants.SunIlluminance = m_lastConstants.SunIlluminance;
    constants.WorldToKilometers = m_worldToKilometers;
    constants.RadianceMultiplier = environmentTint * std::max(environmentIntensity, 0.0f);
    constants.MaxDistanceKm = m_aerialPerspectiveMaxDistanceKm;
    const affine3 skyToWorld = dm::rotation(dm::radians(environmentRotationDeg));
    constants.AtmosphereBasisXWorld = skyToWorld.transformVector(float3(1.0f, 0.0f, 0.0f));
    constants.AtmosphereBasisYWorld = skyToWorld.transformVector(float3(0.0f, 0.0f, 1.0f));
    constants.AtmosphereBasisZWorld = skyToWorld.transformVector(float3(0.0f, 1.0f, 0.0f));
    constants.OutputSize = uint2(width, height);
    constants.ReverseDepth = view.isReverseDepth() ? 1u : 0u;
    constants.SampleCount = (uint)dm::clamp(m_aerialPerspectiveSampleCount, 1, 64);
    commandList->writeBuffer(m_aerialPerspectiveConstantBuffer, &constants, sizeof(constants));

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, m_aerialPerspectiveConstantBuffer),
        caustica::rhi::BindingSetItem::Texture_UAV(0, color),
        caustica::rhi::BindingSetItem::Texture_SRV(0, depth),
        caustica::rhi::BindingSetItem::Texture_SRV(1, m_transmittanceLut),
        caustica::rhi::BindingSetItem::Texture_SRV(2, m_multiScatLut),
        caustica::rhi::BindingSetItem::Sampler(0, m_linearClampSampler),
    };

    // color/depth can change with resolution or render-target swaps; recreate only then.
    const bool needNewBindings = !m_aerialPerspectiveBindings
        || !m_aerialPerspectiveBindings->getDesc()
        || m_aerialPerspectiveBindings->getDesc()->bindings.size() != bindingSetDesc.bindings.size()
        || m_aerialPerspectiveBindings->getDesc()->bindings[1].resourceHandle != color
        || m_aerialPerspectiveBindings->getDesc()->bindings[2].resourceHandle != depth;
    if (needNewBindings)
        m_aerialPerspectiveBindings = m_device->createBindingSet(bindingSetDesc, m_aerialPerspectiveBindingLayout);

    caustica::rhi::ComputeState state;
    state.bindings = { m_aerialPerspectiveBindings };
    state.pipeline = m_aerialPerspectivePSO;
    commandList->setComputeState(state);
    commandList->dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

bool SampleProceduralSky::update(
    caustica::rhi::CommandList* commandList,
    double sceneTime,
    ProceduralSkyConstants& outConstants,
    const std::string& presetType,
    bool forceInstantUpdate)
{
    memset(&outConstants, 0, sizeof(outConstants));
    m_activePresetType = presetType;

    fillEarthAtmosphere(outConstants.Atmosphere);

    outConstants.CameraHeightKm = dm::clamp(
        m_cameraHeightKm,
        0.001f,
        m_atmosphereHeightKm - 0.001f);
    outConstants.SunIlluminance = float3(m_sunBrightness, m_sunBrightness, m_sunBrightness);
    outConstants.SunAngularDiameter = m_sunAngularDiameterDeg / 180.0f * PI_f;
    outConstants.sun_solid_angle = 2 * PI_f * (float)(1.0 - cos(0.5 * outConstants.SunAngularDiameter));

    float elevationTarget = m_sunElevationDeg;
    float azimuthTarget = m_sunAzimuthDeg;
    bool usePresetTargets = false;

    if (presetType == c_EnvMapProcSky_Morning)
    {
        elevationTarget = 18.0f; azimuthTarget = 85.0f; usePresetTargets = true;
    }
    else if (presetType == c_EnvMapProcSky_Midday)
    {
        elevationTarget = 62.0f; azimuthTarget = 180.0f; usePresetTargets = true;
    }
    else if (presetType == c_EnvMapProcSky_Evening)
    {
        elevationTarget = 8.0f; azimuthTarget = 275.0f; usePresetTargets = true;
    }
    else if (presetType == c_EnvMapProcSky_Dawn)
    {
        elevationTarget = 2.0f; azimuthTarget = 80.0f; usePresetTargets = true;
    }
    else if (presetType == c_EnvMapProcSky_PitchBlack)
    {
        elevationTarget = -25.0f; azimuthTarget = m_sunAzimuthDeg; usePresetTargets = true;
        outConstants.SunIlluminance = 0.0f;
    }

    const float deltaTime = (float)caustica::math::clamp(sceneTime - m_lastSceneTime, 0.0, 0.3);
    m_lastSceneTime = sceneTime;

    if (usePresetTargets)
    {
        m_animateSun = false;
        const float lerpK = forceInstantUpdate ? 1.0f : TimeIndependentLerpF(deltaTime, 2.0f);
        m_sunElevationL1 = lerp(m_sunElevationL1, elevationTarget, lerpK);
        m_sunElevationL2 = lerp(m_sunElevationL2, m_sunElevationL1, lerpK);
        m_sunAzimuthL1 = LerpAngleDeg(m_sunAzimuthL1, azimuthTarget, lerpK);
        m_sunAzimuthL2 = LerpAngleDeg(m_sunAzimuthL2, m_sunAzimuthL1, lerpK);
        if (fabsf(elevationTarget - m_sunElevationL2) < 0.01f
            && AngularDistanceDeg(azimuthTarget, m_sunAzimuthL2) < 0.01f)
        {
            m_sunElevationL1 = m_sunElevationL2 = elevationTarget;
            m_sunAzimuthL1 = m_sunAzimuthL2 = WrapDegrees360(azimuthTarget);
        }
        m_sunElevationDeg = m_sunElevationL2;
        m_sunAzimuthDeg = m_sunAzimuthL2;
    }
    else if (m_animateSun)
    {
        // free sky day-cycle: elevation from -max..+max, azimuth swings ±90° around noon bearing.
        const float daysPerSecond = m_sunAnimSpeed / 60.0f;
        m_sunAnimPhase = (float)std::fmod(m_sunAnimPhase + deltaTime * daysPerSecond + 1.0, 1.0);
        const float dayAngle = m_sunAnimPhase * 2.0f * PI_f;
        m_sunElevationDeg = std::sin(dayAngle) * m_sunAnimMaxElevation;
        m_sunAzimuthDeg = WrapDegrees360(m_noonAzimuthDeg + std::cos(dayAngle) * 90.0f);
        m_sunElevationL1 = m_sunElevationL2 = m_sunElevationDeg;
        m_sunAzimuthL1 = m_sunAzimuthL2 = m_sunAzimuthDeg;
    }
    else
    {
        m_sunAzimuthDeg = WrapDegrees360(m_sunAzimuthDeg);
        m_sunElevationL1 = m_sunElevationL2 = m_sunElevationDeg;
        m_sunAzimuthL1 = m_sunAzimuthL2 = m_sunAzimuthDeg;
    }

    outConstants.SunDir = computeSunDirection(m_sunElevationDeg, m_sunAzimuthDeg);

    const bool rebuildAtmosphereLuts = !m_atmosphereLutsValid || !AtmosphereEqual(outConstants.Atmosphere, m_lastAtmosphere);
    const bool rebuildSkyView = rebuildAtmosphereLuts
        || memcmp(&outConstants.SunDir, &m_lastSunDir, sizeof(float3)) != 0
        || memcmp(&outConstants.SunIlluminance, &m_lastSunIlluminance, sizeof(float3)) != 0
        || outConstants.CameraHeightKm != m_lastCameraHeightKm;

    if (commandList && (rebuildAtmosphereLuts || rebuildSkyView))
        dispatchLutPasses(commandList, outConstants, rebuildAtmosphereLuts, rebuildSkyView);

    bool changes = memcmp(&outConstants, &m_lastConstants, sizeof(outConstants)) != 0 || rebuildAtmosphereLuts || rebuildSkyView;
    m_lastConstants = outConstants;
    return changes;
}

bool SampleProceduralSky::debugGUI(float indent)
{
    bool changed = false;
    auto mark = [&](bool v) { changed |= v; };

    ImGui::TextWrapped("Hillaire 2020 Sky Atmosphere — bake into dynamic environment cubemap.");
    RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););

    if (ImGui::CollapsingHeader("Sun Direction", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););

        const bool namedPreset = isProceduralSky(m_activePresetType.c_str())
            && m_activePresetType != c_EnvMapProcSky;
        if (namedPreset)
        {
            ImGui::TextWrapped(
                "A named sky preset is driving the sun direction.\n"
                "Switch Environment Override to 'sky (manual)' for free elevation/azimuth control.");
        }

        ImGui::BeginDisabled(namedPreset);

        mark(ImGui::Checkbox("animate sun (day cycle)", &m_animateSun));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, elevation/azimuth follow a day arc.\nDisable for full manual control.");

        if (m_animateSun)
        {
            mark(ImGui::SliderFloat("Day speed (cycles / min)", &m_sunAnimSpeed, 0.0f, 30.0f, "%.2f"));
            mark(ImGui::SliderFloat("Max elevation (deg)", &m_sunAnimMaxElevation, 5.0f, 89.0f, "%.1f"));
            mark(ImGui::SliderFloat("Noon azimuth (deg)", &m_noonAzimuthDeg, 0.0f, 360.0f, "%.1f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Azimuth at solar noon. Day arc swings ±90° around this bearing.");
            m_noonAzimuthDeg = WrapDegrees360(m_noonAzimuthDeg);
            mark(ImGui::SliderFloat("Day phase", &m_sunAnimPhase, 0.0f, 1.0f, "%.3f"));
        }
        else
        {
            mark(ImGui::SliderFloat("Elevation (deg)", &m_sunElevationDeg, -20.0f, 89.0f, "%.2f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = horizon, 90 = zenith, negative = below horizon.");
            mark(ImGui::SliderFloat("Azimuth (deg)", &m_sunAzimuthDeg, 0.0f, 360.0f, "%.2f"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = +X, 90 = +Y in sky-local (Z-up) space.");
            mark(ImGui::InputFloat("Elevation##in", &m_sunElevationDeg, 1.0f, 5.0f, "%.2f"));
            mark(ImGui::InputFloat("Azimuth##in", &m_sunAzimuthDeg, 1.0f, 15.0f, "%.2f"));
            m_sunElevationDeg = dm::clamp(m_sunElevationDeg, -89.0f, 89.0f);
            m_sunAzimuthDeg = WrapDegrees360(m_sunAzimuthDeg);
        }

        ImGui::Text("Presets:");
        if (ImGui::Button("Sunrise")) { applySunPreset(5.0f, 85.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Morning")) { applySunPreset(25.0f, 100.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Noon")) { applySunPreset(65.0f, 180.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Golden hour")) { applySunPreset(12.0f, 260.0f); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Sunset")) { applySunPreset(3.0f, 275.0f); changed = true; }

        ImGui::EndDisabled();

        const float3 dir = computeSunDirection(m_sunElevationDeg, m_sunAzimuthDeg);
        ImGui::Text("Direction: (%.3f, %.3f, %.3f)  elev=%.1f°  azim=%.1f°",
            dir.x, dir.y, dir.z, m_sunElevationDeg, m_sunAzimuthDeg);
    }

    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        mark(ImGui::DragFloat("Sun illuminance", &m_sunBrightness, 0.01f, 0.0f, 64.0f));
        mark(ImGui::SliderFloat("Multi-scattering", &m_multiScatteringFactor, 0.0f, 2.0f));
        mark(ImGui::DragFloat("Sun angular diameter (deg)", &m_sunAngularDiameterDeg, 0.01f, 0.05f, 5.0f));
        mark(ImGui::DragFloat("Camera height (km)", &m_cameraHeightKm, 0.01f, 0.001f, 50.0f));
    }

    if (ImGui::CollapsingHeader("Atmosphere / Aerosols", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        ImGui::TextWrapped("Physical density and aerosol controls. Changes rebuild atmosphere LUTs.");

        mark(ImGui::DragFloat("Atmosphere height (km)", &m_atmosphereHeightKm, 0.1f, 20.0f, 300.0f));
        mark(ImGui::SliderFloat("Rayleigh scattering", &m_rayleighScatteringScale, 0.0f, 4.0f, "%.3f"));
        mark(ImGui::DragFloat("Rayleigh scale height (km)", &m_rayleighHeightKm, 0.05f, 1.0f, 30.0f));

        ImGui::SeparatorText("Mie aerosols");
        mark(ImGui::SliderFloat("Mie scattering", &m_mieScatteringScale, 0.0f, 10.0f, "%.3f"));
        mark(ImGui::SliderFloat("Mie absorption", &m_mieAbsorptionScale, 0.0f, 10.0f, "%.3f"));
        mark(ImGui::DragFloat("Mie scale height (km)", &m_mieHeightKm, 0.01f, 0.1f, 10.0f));
        mark(ImGui::SliderFloat("Mie anisotropy", &m_mieAnisotropy, 0.0f, 0.99f, "%.3f"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher values create a tighter forward-scattering halo around the sun.");

        ImGui::SeparatorText("Absorption / ground");
        mark(ImGui::SliderFloat("Ozone absorption", &m_ozoneScale, 0.0f, 4.0f, "%.3f"));
        mark(ImGui::ColorEdit3("Ground albedo", &m_groundAlbedo.x, ImGuiColorEditFlags_Float));

        if (ImGui::Button("reset Earth atmosphere"))
        {
            m_atmosphereHeightKm = 100.0f;
            m_rayleighScatteringScale = 1.0f;
            m_rayleighHeightKm = 8.0f;
            m_mieScatteringScale = 1.0f;
            m_mieAbsorptionScale = 1.0f;
            m_mieHeightKm = 1.2f;
            m_mieAnisotropy = 0.8f;
            m_ozoneScale = 1.0f;
            m_groundAlbedo = float3(0.3f, 0.3f, 0.3f);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Aerial Perspective", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent););
        mark(ImGui::Checkbox("Enable aerial perspective", &m_aerialPerspectiveEnabled));
        ImGui::BeginDisabled(!m_aerialPerspectiveEnabled);
        mark(ImGui::DragFloat("World units to km", &m_worldToKilometers, 0.00001f, 0.000001f, 1.0f, "%.6f"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Default 0.001 assumes one world unit is one meter.");
        mark(ImGui::DragFloat("Aerial max distance (km)", &m_aerialPerspectiveMaxDistanceKm, 0.5f, 0.1f, 1000.0f));
        mark(ImGui::SliderInt("Aerial samples", &m_aerialPerspectiveSampleCount, 4, 32));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher values improve long-distance quality but increase full-screen cost.");
        ImGui::EndDisabled();
    }

    return changed;
}
