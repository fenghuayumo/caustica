#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <math/math.h>

// ToneMapping_cb.h uses HLSL-style uint / float3x4; bind them before include.
using caustica::math::uint;
using caustica::math::float3;
using caustica::math::float4;
using caustica::math::float3x4;

#include <shaders/render/toneMapper/ToneMapping_cb.h>

// Scene JSON / camera metadata use these ids. UI labels are separate
// (tonemapOperatorToString) and must not be written back to the camera —
// display names like "Reinhard Modified" fail to parse and snap to Aces.
inline const char* toneMapOperatorId(ToneMapperOperator op)
{
    switch (op)
    {
    case ToneMapperOperator::Linear:               return "Linear";
    case ToneMapperOperator::Reinhard:             return "Reinhard";
    case ToneMapperOperator::ReinhardModified:     return "ReinhardModified";
    case ToneMapperOperator::HejiHableAlu:         return "HejiHableAlu";
    case ToneMapperOperator::HableUc2:             return "HableUc2";
    case ToneMapperOperator::Aces:                 return "Aces";
    case ToneMapperOperator::PbrNeutral:           return "PbrNeutral";
    case ToneMapperOperator::IdentitySoftShoulder: return "IdentitySoftShoulder";
    case ToneMapperOperator::AgX:                  return "AgX";
    case ToneMapperOperator::CameraLut:            return "CameraLut";
    }
    return "Aces";
}

inline bool tryParseToneMapOperator(std::string_view name, ToneMapperOperator& out)
{
    if (name == "Linear") { out = ToneMapperOperator::Linear; return true; }
    if (name == "Reinhard") { out = ToneMapperOperator::Reinhard; return true; }
    if (name == "ReinhardModified" || name == "Reinhard Modified")
    {
        out = ToneMapperOperator::ReinhardModified;
        return true;
    }
    if (name == "HejiHableAlu" || name == "Heji Hable ALU")
    {
        out = ToneMapperOperator::HejiHableAlu;
        return true;
    }
    if (name == "HableUc2" || name == "Hable UC2")
    {
        out = ToneMapperOperator::HableUc2;
        return true;
    }
    if (name == "Aces") { out = ToneMapperOperator::Aces; return true; }
    if (name == "PbrNeutral" || name == "KhronosPbrNeutral"
        || name == "Khronos PBR Neutral" || name == "Neutral")
    {
        out = ToneMapperOperator::PbrNeutral;
        return true;
    }
    if (name == "IdentitySoftShoulder" || name == "Identity + Soft Shoulder")
    {
        out = ToneMapperOperator::IdentitySoftShoulder;
        return true;
    }
    if (name == "AgX") { out = ToneMapperOperator::AgX; return true; }
    if (name == "CameraLut" || name == "Camera LUT")
    {
        out = ToneMapperOperator::CameraLut;
        return true;
    }
    return false;
}

enum class ExposureMode : uint32_t
{
    AperturePriority,
    ShutterPriority,
};

enum class CameraLutPreset : uint32_t
{
    None,
    Neutral,
    SoftContrast,
    WarmFilm,
    CoolFilm,
    CustomFile,
};

struct ToneMappingParameters
{
    static constexpr uint32_t CameraLutSize = 256;

    ExposureMode exposureMode = ExposureMode::AperturePriority;
    ToneMapperOperator toneMapOperator = ToneMapperOperator::Aces;
    bool autoExposure = false;
    float exposureCompensation = 0.0f;
    float exposureValue = 0.0f;
    float filmSpeed = 100.f;
    float fNumber = 1.f;
    float shutter = 1.f;
    bool whiteBalance = false;
    float whitePoint = 6500.0f;
    float whiteMaxLuminance = 1.0f;
    float whiteScale = 5.1f;
    bool clamped = true;
    float exposureValueMin = -16.0f;
    float exposureValueMax = 16.0f;

    // Per-channel camera response stored in linear Rec.709. The curve is
    // applied after the selected tone mapper and before framebuffer encoding.
    bool cameraLutEnabled = false;
    bool cameraLutAfterToneMap = true;
    CameraLutPreset cameraLutPreset = CameraLutPreset::None;
    bool cameraLutIs3D = false;
    caustica::math::float3 cameraLutDomainMin = caustica::math::float3(0.0f);
    caustica::math::float3 cameraLutDomainMax = caustica::math::float3(1.0f);
    std::array<caustica::math::float3, CameraLutSize> cameraLut = [] {
        std::array<caustica::math::float3, CameraLutSize> result{};
        for (uint32_t i = 0; i < CameraLutSize; ++i)
        {
            const float x = static_cast<float>(i) / static_cast<float>(CameraLutSize - 1);
            result[i] = caustica::math::float3(x);
        }
        return result;
    }();
    std::string cameraLutPath;
    uint32_t cameraLut3DSize = 0;
    std::shared_ptr<const std::vector<caustica::math::float4>> cameraLut3D;
    uint64_t cameraLutRevision = 0;

    // Loads standard LUT_1D_SIZE or LUT_3D_SIZE .cube files. 1D tables are
    // resampled to 256 entries; 3D tables retain their original resolution.
    bool loadCameraLut(const std::string& path, std::string* error = nullptr);
    void applyCameraLutPreset(CameraLutPreset preset);
    void clearCameraLut();
};

static const std::unordered_map<ExposureMode, std::string> ExposureModeToString = {
    {ExposureMode::AperturePriority, "Aperture Priority"},
    {ExposureMode::ShutterPriority, "Shutter Priority"}
};

static const std::unordered_map<CameraLutPreset, std::string> CameraLutPresetToString = {
    {CameraLutPreset::None, "None (disabled)"},
    {CameraLutPreset::Neutral, "Neutral"},
    {CameraLutPreset::SoftContrast, "Soft Contrast"},
    {CameraLutPreset::WarmFilm, "Warm Film"},
    {CameraLutPreset::CoolFilm, "Cool Film"},
    {CameraLutPreset::CustomFile, "Custom .cube"},
};
